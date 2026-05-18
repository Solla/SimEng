#include "simeng/pipeline/FetchUnit.hh"

namespace simeng {
namespace pipeline {

FetchUnit::FetchUnit(PipelineBuffer<MacroOp>& output,
                     MemoryInterface& instructionMemory, uint8_t blockSize,
                     const arch::Architecture& isa,
                     BranchPredictor& branchPredictor)
    : output_(output),
      instructionMemory_(instructionMemory),
      isa_(isa),
      branchPredictor_(branchPredictor),
      blockSize_(blockSize),
      blockMask_(~(blockSize_ - 1)) {
  assert(blockSize_ >= isa_.getMaxInstructionSize() &&
         "fetch block size must be larger than the largest instruction");
  fetchBuffer_ = new uint8_t[2 * blockSize_];
}

FetchUnit::~FetchUnit() { delete[] fetchBuffer_; }

void FetchUnit::tick() {
  if (programByteLength_ == 0) {
    std::cerr
        << "[SimEng::FetchUnit] Invalid Program Byte Length of 0. Please "
           "ensure setProgramLength() is called before calling updatePC().\n";
    exit(1);
  }

  if (output_.isStalled() || hasHalted_ || paused_) {
    return;
  }

  // If loop buffer has been filled, fill buffer to decode
  if (loopBufferState_ == LoopBufferState::SUPPLYING) {
    auto outputSlots = output_.getTailSlots();
    for (size_t slot = 0; slot < output_.getWidth(); slot++) {
      auto& macroOp = outputSlots[slot];
      auto bytesRead = isa_.predecode(&(loopBuffer_.front().encoding),
                                      loopBuffer_.front().instructionSize,
                                      loopBuffer_.front().address, macroOp);

      assert(bytesRead != 0 && "predecode failure for loop buffer entry");
      (void)bytesRead;
      lbSupplyInstrs_++;

      // Still consult the branch predictor for loop-buffered branches. The
      // predictor follows an FTQ protocol: every branch reaching the ROB is
      // matched by exactly one predict() (ftq push / history advance) and one
      // update()/flush(). Reusing the recorded prediction without calling
      // predict() would commit/flush ftq entries that were never pushed,
      // desyncing the FTQ and global history (catastrophic for loop-heavy
      // code such as Dhrystone). The loop buffer remains a fetch-bandwidth
      // optimisation only; control flow is still driven by loopBuffer_.
      bool loopExitPredicted = false;
      if (macroOp[0]->isBranch()) {
        BranchPrediction prediction = branchPredictor_.predict(
            loopBuffer_.front().address, macroOp[0]->getBranchType(),
            macroOp[0]->getKnownOffset());
        macroOp[0]->setBranchPrediction(prediction);
        // Symmetric to the FILLING state (see the loopBoundaryAddress_ check
        // there): if the loop-closing branch is predicted to leave the loop,
        // stop supplying from the buffer and resume normal fetch at the
        // fall-through. Without this, SUPPLYING cycles the buffer
        // indefinitely and is only ever broken by a downstream misprediction
        // flush; the unbounded speculative predict() calls then accumulate
        // FTQ/global-history desync that eventually wedges a later loop
        // (observed: nolibc Dhrystone Proc0 strcpy loop running away after
        // ~10 invocations under the default loop-buffer config).
        if (loopBuffer_.front().address == loopBoundaryAddress_ &&
            !prediction.isTaken) {
          loopExitPredicted = true;
        }
      }

      if (loopExitPredicted) {
        // Fall through past the loop-closing branch; the branch macro-op for
        // this slot has been emitted and will resolve in the ROB as usual.
        pc_ = loopBuffer_.front().address +
              loopBuffer_.front().instructionSize;
        loopBuffer_.clear();
        loopBufferState_ = LoopBufferState::IDLE;
        loopBoundaryAddress_ = 0;
        if (onLoopBufferIdle_) onLoopBufferIdle_();
        return;
      }

      // Cycle queue by moving front entry to back
      loopBuffer_.push_back(loopBuffer_.front());
      loopBuffer_.pop_front();
      // Update PC to address of next instruction in buffer to maintain correct
      // PC value
      pc_ = loopBuffer_.front().address;
    }
    return;
  }

  // Pointer to the instruction data to decode from
  const uint8_t* buffer;
  uint8_t bufferOffset;

  // Check if more instruction data is required
  if (bufferedBytes_ < isa_.getMaxInstructionSize()) {
    // Calculate the address of the next fetch block
    uint64_t blockAddress;
    if (bufferedBytes_ > 0) {
      // There is already some data in the buffer, so check for the next block
      bufferOffset = 0;
      blockAddress = pc_ + bufferedBytes_;
      assert((blockAddress & ~blockMask_) == 0 && "misaligned fetch buffer");
    } else {
      // Fetch buffer is empty, so start from the PC
      blockAddress = pc_ & blockMask_;
      bufferOffset = pc_ - blockAddress;
    }

    // Find fetched memory that matches the desired block
    const auto& fetched = instructionMemory_.getCompletedReads();

    size_t fetchIndex;
    for (fetchIndex = 0; fetchIndex < fetched.size(); fetchIndex++) {
      // A data null check "fetched[fetchIndex].data" is added to handle empty
      // fetched instructions that are caused by incorrectly speculated branch
      // instructions. Wrongly speculated branch instructions can sometimes
      // generate addresses that have no mapping in the PageTable. This causes a
      // page table fault which is handled by the OS. Since the address related
      // to the branch instruction can be a garbage address to a region of
      // memory which cannot be mapped, a data abort exception is thrown and an
      // empty register value is returned as the read payload. A data null check
      // suffices to catch these data aborts.
      if (fetched[fetchIndex].target.address == blockAddress &&
          fetched[fetchIndex].data) {
        break;
      }
    }
    if (fetchIndex == fetched.size()) {
      // Need to wait for fetched instructions
      return;
    }

    // TODO: Handle memory faults
    const uint8_t* fetchData = fetched[fetchIndex].data.getAsVector<uint8_t>();

    // Copy fetched data to fetch buffer after existing data
    std::memcpy(fetchBuffer_ + bufferedBytes_, fetchData + bufferOffset,
                blockSize_ - bufferOffset);

    bufferedBytes_ += blockSize_ - bufferOffset;
    buffer = fetchBuffer_;
    // Decoding should start from the beginning of the fetchBuffer_.
    bufferOffset = 0;
  } else {
    // There is already enough data in the fetch buffer, so use that
    buffer = fetchBuffer_;
    bufferOffset = 0;
  }

  // Check we have enough data to begin decoding
  if (bufferedBytes_ < isa_.getMaxInstructionSize()) return;

  auto outputSlots = output_.getTailSlots();
  for (size_t slot = 0; slot < output_.getWidth(); slot++) {
    auto& macroOp = outputSlots[slot];

    auto bytesRead =
        isa_.predecode(buffer + bufferOffset, bufferedBytes_, pc_, macroOp);

    // If predecode fails, bail and wait for more data
    if (bytesRead == 0) {
      assert(bufferedBytes_ < isa_.getMaxInstructionSize() &&
             "unexpected predecode failure");
      break;
    }

    // Create branch prediction after identifing instruction type
    // (e.g. RET, BL, etc).
    BranchPrediction prediction = {false, 0};
    if (macroOp[0]->isBranch()) {
      prediction = branchPredictor_.predict(pc_, macroOp[0]->getBranchType(),
                                            macroOp[0]->getKnownOffset());
      macroOp[0]->setBranchPrediction(prediction);
    }

    if (loopBufferState_ == LoopBufferState::FILLING) {
      // Record instruction fetch information in loop body
      uint32_t encoding;
      memcpy(&encoding, buffer + bufferOffset, sizeof(uint32_t));
      loopBuffer_.push_back(
          {encoding, bytesRead, pc_, macroOp[0]->getBranchPrediction()});

      // The SUPPLYING state replays the captured body linearly and only
      // honours control flow for the single loop-closing branch
      // (loopBoundaryAddress_). If the body contains ANY other branch
      // (an internal conditional branch, an unconditional branch, a
      // call or a return), linear replay supplies the captured
      // fall-through regardless of that branch's actual direction. When
      // such a branch's prediction matches its real outcome there is no
      // rescuing misprediction flush, so the wrong instructions retire
      // and silently corrupt architectural state (observed: CoreMark
      // matrix_sum / list / state CRCs wrong only with the loop buffer
      // engaged; correct with it disabled). Only single-basic-block
      // loops are safe to buffer, so abort buffering this loop if a
      // non-boundary branch appears in its body and fall back to normal
      // fetch.
      if (macroOp[0]->isBranch() && pc_ != loopBoundaryAddress_) {
        lbFillAbortedBranch_++;
        loopBuffer_.clear();
        loopBufferState_ = LoopBufferState::IDLE;
        loopBoundaryAddress_ = 0;
        if (onLoopBufferIdle_) onLoopBufferIdle_();
      } else if (pc_ == loopBoundaryAddress_) {
        if (macroOp[0]->isBranch() &&
            !macroOp[0]->getBranchPrediction().isTaken) {
          lbFillAbortedExit_++;
          // loopBoundaryAddress_ has been fetched whilst filling the loop
          // buffer BUT this is a branch, predicted to branch out of the loop
          // being buffered. Stop filling the loop buffer and don't supply to
          // decode
          loopBufferState_ = LoopBufferState::IDLE;
          if (onLoopBufferIdle_) onLoopBufferIdle_();
        } else {
          // loopBoundaryAddress_ has been fetched whilst filling the loop
          // buffer. Stop filling as loop body has been recorded and begin to
          // supply decode unit with instructions from the loop buffer
          loopBufferState_ = LoopBufferState::SUPPLYING;
          lbSupplyEntered_++;
          bufferedBytes_ = 0;
          break;
        }
      }
    } else if (loopBufferState_ == LoopBufferState::WAITING &&
               pc_ == loopBoundaryAddress_) {
      // Once set loopBoundaryAddress_ is fetched, start to fill loop buffer
      loopBufferState_ = LoopBufferState::FILLING;
      lbFillStarted_++;
    }

    assert(bytesRead <= bufferedBytes_ &&
           "Predecode consumed more bytes than were available");
    // Increment the offset, decrement available bytes
    bufferOffset += bytesRead;
    bufferedBytes_ -= bytesRead;

    if (!prediction.isTaken) {
      // Predicted as not taken; increment PC to next instruction
      pc_ += bytesRead;
    } else {
      // Predicted as taken; set PC to predicted target address
      pc_ = prediction.target;
    }

    if (pc_ >= programByteLength_) {
      hasHalted_ = true;
      break;
    }

    if (prediction.isTaken) {
      // Diagnostic: a predicted-taken branch always truncates the fetch
      // group here. Attribute it to its branch type and tally the empty
      // trailing slots it cost this cycle (pure observation).
      size_t bt = static_cast<size_t>(macroOp[0]->getBranchType());
      if (bt >= takenByType_.size()) bt = takenByType_.size() - 1;
      takenByType_[bt]++;
      if (slot + 1 < output_.getWidth()) {
        branchStalls_++;
        stallByType_[bt]++;
        branchStallSlots_ += (output_.getWidth() - 1 - slot);
      }
      // Can't continue fetch immediately after a branch
      bufferedBytes_ = 0;
      break;
    }

    // Too few bytes remaining in buffer to continue
    if (bufferedBytes_ == 0) {
      break;
    }
  }

  if (bufferedBytes_ > 0) {
    // Move start of fetched data to beginning of fetch buffer
    std::memmove(fetchBuffer_, buffer + bufferOffset, bufferedBytes_);
  }

  instructionMemory_.clearCompletedReads();
}

void FetchUnit::registerLoopBoundary(uint64_t branchAddress) {
  // Set branch which forms the loop as the loopBoundaryAddress_ and place loop
  // buffer in state to begin filling once the loopBoundaryAddress_ has been
  // fetched
  loopBufferState_ = LoopBufferState::WAITING;
  loopBoundaryAddress_ = branchAddress;
}

bool FetchUnit::hasHalted() const { return hasHalted_; }

void FetchUnit::updatePC(uint64_t address) {
  pc_ = address;
  bufferedBytes_ = 0;
  if (programByteLength_ == 0) {
    std::cerr
        << "[SimEng::FetchUnit] Invalid Program Byte Length of 0. Please "
           "ensure setProgramLength() is called before calling updatePC().\n";
    exit(1);
  }
  hasHalted_ = (pc_ >= programByteLength_);
}

void FetchUnit::setProgramLength(uint64_t size) { programByteLength_ = size; }

void FetchUnit::requestFromPC() {
  // Do nothing if paused
  if (paused_) return;

  // Do nothing if buffer already contains enough data
  if (bufferedBytes_ >= isa_.getMaxInstructionSize()) return;

  // Do nothing if unit has halted to avoid invalid speculative memory reads
  // beyond the programByteLength_
  if (hasHalted_) return;

  uint64_t blockAddress;
  if (bufferedBytes_ > 0) {
    // There's already some data in the buffer, so fetch the next block
    blockAddress = pc_ + bufferedBytes_;
    assert((blockAddress & ~blockMask_) == 0 && "misaligned fetch buffer");
  } else {
    // Fetch buffer is empty, so fetch from the PC
    blockAddress = pc_ & blockMask_;
  }

  instructionMemory_.requestRead({blockAddress, blockSize_});
}

uint64_t FetchUnit::getBranchStalls() const { return branchStalls_; }

std::vector<std::pair<std::string, std::string>> FetchUnit::getFetchProfile()
    const {
  // BranchType order: Conditional, LoopClosing, Return, SubroutineCall,
  // Unconditional, Unknown.
  static const char* kTypeName[6] = {"cond", "loopClose", "ret",
                                     "call", "uncond",    "unknown"};
  std::vector<std::pair<std::string, std::string>> p;
  for (size_t i = 0; i < 6; i++)
    p.emplace_back(std::string("fetchprof.taken.") + kTypeName[i],
                   std::to_string(takenByType_[i]));
  for (size_t i = 0; i < 6; i++)
    p.emplace_back(std::string("fetchprof.stall.") + kTypeName[i],
                   std::to_string(stallByType_[i]));
  p.emplace_back("fetchprof.stallSlots", std::to_string(branchStallSlots_));
  p.emplace_back("fetchprof.lb.fillStarted", std::to_string(lbFillStarted_));
  p.emplace_back("fetchprof.lb.fillAbortedBranch",
                 std::to_string(lbFillAbortedBranch_));
  p.emplace_back("fetchprof.lb.fillAbortedExit",
                 std::to_string(lbFillAbortedExit_));
  p.emplace_back("fetchprof.lb.supplyEntered",
                 std::to_string(lbSupplyEntered_));
  p.emplace_back("fetchprof.lb.supplyInstrs",
                 std::to_string(lbSupplyInstrs_));
  return p;
}

void FetchUnit::flushLoopBuffer() {
  loopBuffer_.clear();
  loopBufferState_ = LoopBufferState::IDLE;
  loopBoundaryAddress_ = 0;
}

}  // namespace pipeline
}  // namespace simeng
