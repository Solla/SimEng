#include "simeng/pipeline/ReorderBuffer.hh"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <utility>
#include <vector>
#include <cassert>
#include <iostream>

namespace simeng {
namespace pipeline {

ReorderBuffer::ReorderBuffer(
    unsigned int maxSize, RegisterAliasTable& rat, LoadStoreQueue& lsq,
    std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
    std::function<void(uint64_t branchAddress)> sendLoopBoundary,
    BranchPredictor& predictor, uint16_t loopBufSize,
    uint16_t loopDetectionThreshold)
    : rat_(rat),
      lsq_(lsq),
      maxSize_(maxSize),
      raiseException_(raiseException),
      predictor_(predictor),
      sendLoopBoundary_(sendLoopBoundary),
      loopBufSize_(loopBufSize),
      loopDetectionThreshold_(loopDetectionThreshold) {
  branchProfileEnabled_ = (std::getenv("SIMENG_BRANCH_PROFILE") != nullptr);
  holProfileEnabled_ = (std::getenv("SIMENG_ROB_HOL_PROFILE") != nullptr);
}

constexpr const char* ReorderBuffer::holClassName_[6];

void ReorderBuffer::reserve(const std::shared_ptr<Instruction>& insn) {
  assert(buffer_.size() < maxSize_ &&
         "Attempted to reserve entry in reorder buffer when already full");
  insn->setSequenceId(seqId_);
  seqId_++;
  insn->setInstructionId(insnId_);
  if (insn->isLastMicroOp()) insnId_++;

  buffer_.push_back(insn);
}

void ReorderBuffer::commitMicroOps(uint64_t insnId) {
  if (buffer_.size()) {
    size_t index = 0;
    int firstOp = -1;
    bool validForCommit = false;

    // Find first instance of uop belonging to macro-op instruction
    for (; index < buffer_.size(); index++) {
      if (buffer_[index]->getInstructionId() == insnId) {
        firstOp = index;
        break;
      }
    }

    if (firstOp > -1) {
      // If found, see if all uops are committable
      for (; index < buffer_.size(); index++) {
        if (buffer_[index]->getInstructionId() != insnId) break;
        if (!buffer_[index]->isWaitingCommit()) {
          return;
        } else if (buffer_[index]->isLastMicroOp()) {
          // all microOps must be in ROB for the commit to be valid
          validForCommit = true;
        }
      }
      if (!validForCommit) return;

      // No early return thus all uops are committable
      for (; firstOp < buffer_.size(); firstOp++) {
        if (buffer_[firstOp]->getInstructionId() != insnId) break;
        buffer_[firstOp]->setCommitReady();
      }
    }
  }
  return;
}

unsigned int ReorderBuffer::commit(uint64_t maxCommitSize) {
  shouldFlush_ = false;
  size_t maxCommits =
      std::min(static_cast<size_t>(maxCommitSize), buffer_.size());

  unsigned int n;
  for (n = 0; n < maxCommits; n++) {
    auto& uop = buffer_.front();
    if (!uop->canCommit()) {
      // Probe 5: classify why ROB head can't commit this cycle.
      if (holProfileEnabled_) {
        int klass;
        if (!uop->hasExecuted()) {
          if (!uop->canExecute()) {
            klass = 0;  // dep_wait
          } else if (uop->isBranch()) {
            klass = 1;  // branch_unresolved
          } else if (uop->isLoad() || uop->isStoreAddress()) {
            klass = 2;  // LSQ_pending
          } else {
            klass = 3;  // exec_in_flight
          }
        } else if (uop->isWaitingCommit()) {
          // Writeback ran; uop is a micro-op holding the head until siblings
          // of the same macro-op finish.
          klass = 4;  // microop_sibling
        } else {
          // EU has executed_=true but WritebackUnit::tick has not yet
          // processed this completion slot. Inherent 1-cycle pipeline lag.
          klass = 5;  // wb_lag
        }
        // Charge a full cycle's worth of HOL stall to this class only once
        // (the outer caller invokes commit() once per cycle). Count only on
        // the first break-iteration of this call.
        if (n == 0) holCycleByClass_[klass]++;
        // Exact PC for microop_sibling (klass 4) and wb_lag (klass 5);
        // 64B-bucket for the others to keep map sizes bounded.
        uint64_t pcKey = (klass >= 4) ? uop->getInstructionAddress()
                                      : (uop->getInstructionAddress() & ~0x3FULL);
        holPcByClass_[klass][pcKey]++;
      }
      break;
    }

    if (uop->isLastMicroOp()) {
      instructionsCommitted_++;
      uint64_t pc = uop->getInstructionAddress();
      lastCommittedPC_ = pc;
      if (pc < 0x4004e0) pcBucket_[0]++;          // pre-main
      else if (pc < 0x401000) pcBucket_[1]++;     // main/dhry funcs
      else if (pc < 0x500000) pcBucket_[2]++;     // libc / etc
      else pcBucket_[3]++;                         // dynamic/heap/other
      // Optional sample of distinct PCs (env-gated) to spot infinite loops.
      if (std::getenv("SIMENG_PC_SAMPLE") != nullptr) {
        static std::map<uint64_t, uint64_t> hist;
        hist[pc & ~0x3FULL]++;
        static uint64_t last_dump = 0;
        if (instructionsCommitted_ - last_dump >= 10000000) {
          last_dump = instructionsCommitted_;
          std::cerr << "[pc-sample] top buckets at retired=" << instructionsCommitted_ << ":\n";
          std::vector<std::pair<uint64_t,uint64_t>> sorted(hist.begin(), hist.end());
          std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b){ return a.second > b.second; });
          for (int i = 0; i < 5 && i < (int)sorted.size(); i++) {
            std::cerr << "  0x" << std::hex << sorted[i].first << std::dec
                      << " : " << sorted[i].second << "\n";
          }
        }
      }
    }

    if (uop->exceptionEncountered()) {
      raiseException_(uop);
      buffer_.pop_front();
      return n + 1;
    }

    const auto& destinations = uop->getDestinationRegisters();
    for (int i = 0; i < destinations.size(); i++) {
      rat_.commit(destinations[i]);
    }

    // If it's a memory op, commit the entry at the head of the respective queue
    if (uop->isLoad()) {
      lsq_.commitLoad(uop);
    }
    if (uop->isStoreAddress()) {
      bool violationFound = lsq_.commitStore(uop);
      if (violationFound) {
        loadViolations_++;
        // Memory order violation found; aborting commits and flushing
        auto load = lsq_.getViolatingLoad();
        shouldFlush_ = true;
        flushAfter_ = load->getInstructionId() - 1;
        pc_ = load->getInstructionAddress();

        buffer_.pop_front();
        return n + 1;
      }
    }

    // Update branch predictor with committed branch outcome
    if (uop->isBranch()) {
      predictor_.update(uop->getInstructionAddress(), uop->wasBranchTaken(),
                        uop->getBranchAddress(), uop->getBranchType(),
                        uop->getInstructionId());
      retiredBranches_++;
      const auto& pred = uop->getBranchPrediction();
      bool mispred = (pred.isTaken != uop->wasBranchTaken() ||
                      pred.target != uop->getBranchAddress());
      if (mispred) {
        branchMispredicts_++;
      }
      // Probe 1: per-PC branch resolution counters (total + misses).
      if (branchProfileEnabled_) {
        auto& e = branchMissByPc_[uop->getInstructionAddress()];
        e.first++;
        if (mispred) e.second++;
      }
    }

    // Increment or swap out branch counter for loop detection
    if (uop->isBranch()) {
      if (!loopDetected_) {
        bool increment = true;
        if (branchCounter_.first.address != uop->getInstructionAddress()) {
          // Mismatch on instruction address, reset
          increment = false;
        } else if (branchCounter_.first.outcome != uop->getBranchPrediction()) {
          // Mismatch on branch outcome, reset
          increment = false;
        } else if ((instructionsCommitted_ -
                    branchCounter_.first.commitNumber) > loopBufSize_) {
          // Loop too big to fit in loop buffer, reset
          increment = false;
        }

        if (increment) {
          // Reset commitNumber value
          branchCounter_.first.commitNumber = instructionsCommitted_;
          // Increment counter
          branchCounter_.second++;

          if (branchCounter_.second > loopDetectionThreshold_) {
            // If the same branch with the same outcome is sequentially retired
            // more times than the loopDetectionThreshold_ value, identify as a
            // loop boundary
            loopDetected_ = true;
            sendLoopBoundary_(uop->getInstructionAddress());
          }
        } else {
          // Swap out latest branch
          branchCounter_ = {
              {uop->getInstructionAddress(), uop->getBranchPrediction(),
               instructionsCommitted_},
              0};
        }
      }
    }
    buffer_.pop_front();
  }

  return n;
}

void ReorderBuffer::flush(uint64_t afterSeqId) {
  // Iterate backwards from the tail of the queue to find and remove ops newer
  // than `afterSeqId`
  while (!buffer_.empty()) {
    auto& uop = buffer_.back();
    if (uop->getInstructionId() <= afterSeqId) {
      break;
    }

    // To rewind destination registers in correct history order, rewinding of
    // register renaming is done backwards
    auto destinations = uop->getDestinationRegisters();
    for (int i = destinations.size() - 1; i >= 0; i--) {
      const auto& reg = destinations[i];
      rat_.rewind(reg);
    }
    uop->setFlushed();
    // If the instruction is a branch, supply address to branch flushing logic
    if (uop->isBranch()) {
      predictor_.flush(uop->getInstructionAddress());
    }
    buffer_.pop_back();
  }

  // Reset branch counter and loop detection
  branchCounter_ = {{0, {false, 0}, 0}, 0};
  loopDetected_ = false;
}

void ReorderBuffer::flush() {
  buffer_ = std::deque<std::shared_ptr<Instruction>>();
  shouldFlush_ = false;
  // Reset branch counter and loop detection
  branchCounter_ = {{0, {false, 0}, 0}, 0};
  loopDetected_ = false;
}

void ReorderBuffer::resetLoopDetection() {
  branchCounter_ = {{0, {false, 0}, 0}, 0};
  loopDetected_ = false;
}

unsigned int ReorderBuffer::size() const { return buffer_.size(); }

unsigned int ReorderBuffer::getFreeSpace() const {
  return maxSize_ - buffer_.size();
}

bool ReorderBuffer::shouldFlush() const { return shouldFlush_; }
uint64_t ReorderBuffer::getFlushAddress() const { return pc_; }
uint64_t ReorderBuffer::getFlushInsnId() const { return flushAfter_; }

uint64_t ReorderBuffer::getInstructionsCommittedCount() const {
  return instructionsCommitted_;
}

uint64_t ReorderBuffer::getViolatingLoadsCount() const {
  return loadViolations_;
}

uint64_t ReorderBuffer::getBranchMispredictedCount() const {
  return branchMispredicts_;
}

uint64_t ReorderBuffer::getRetiredBranchesCount() const {
  return retiredBranches_;
}

}  // namespace pipeline
}  // namespace simeng
