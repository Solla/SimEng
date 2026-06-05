#include "simeng/branchpredictors/PerceptronPredictor.hh"
#include <cmath>

namespace simeng {

PerceptronPredictor::PerceptronPredictor(ryml::ConstNodeRef config)
    : btbBits_(config["Branch-Predictor"]["BTB-Tag-Bits"].as<uint64_t>()),
      globalHistoryLength_(
          config["Branch-Predictor"]["Global-History-Length"].as<uint64_t>()),
      rasSize_(config["Branch-Predictor"]["RAS-entries"].as<uint64_t>()) {
  // Build BTB based on config options
  uint32_t btbSize = (1 << btbBits_);
  btb_.resize(btbSize);
  // Initialise perceptron values with 0 for the global history weights, and 1
  // for the bias weight; and initialise the target with 0 (i.e., unknown)
  for (uint64_t i = 0; i < btbSize; i++) {
    btb_[i].first.assign(globalHistoryLength_, 0);
    btb_[i].first.push_back(1);
    btb_[i].second = 0;
  }

  // Set up training threshold according to empirically determined formula
  trainingThreshold_ = (uint64_t)((1.93 * globalHistoryLength_) + 14);

  // NOTE: `(len * 2) - 1` is not the algebraically-correct bit-mask for a
  // `len`-bit GHR (that would be `(1 << len) - 1`). With len=19 it yields 37 =
  // 0b100101, retaining only bits 0/2/5 of GHR — the predictor behaves as a
  // sparse 3-input perceptron, not 19. This is kept deliberately: widening the
  // mask to the full window REGRESSED both workloads (the 19-input perceptron
  // is under-trained over the real branch stream), and the sparse effective
  // history is what the rest of the model is tuned against. (2026-06-05: the
  // full-mask and contiguous-mask alternatives were probed and refuted.)
  globalHistoryMask_ = (globalHistoryLength_ * 2) - 1;
}

PerceptronPredictor::~PerceptronPredictor() {
  ras_.clear();
  rasHistory_.clear();
  FTQ_.clear();
}

BranchPrediction PerceptronPredictor::predict(uint64_t address, BranchType type,
                                              int64_t knownOffset) {
  // Get the hashed index for the prediction table.  XOR the global history with
  // the non-zero bits of the address, and then keep only the btbBits_ bits of
  // the output to keep it in bounds of the prediction table.
  uint64_t hashedIndex =
      ((address >> 2) ^ globalHistory_) & ((1 << btbBits_) - 1);

  // Retrieve the perceptron from the BTB
  std::vector<int8_t> perceptron = btb_[hashedIndex].first;

  // Get dot product of perceptron and history
  int64_t Pout = getDotProduct(perceptron, globalHistory_);
  // Determine direction prediction based on its sign
  bool direction = (Pout >= 0);

  // Retrieve target prediction
  uint64_t target =
      (knownOffset != 0) ? address + knownOffset : btb_[hashedIndex].second;

  BranchPrediction prediction = {direction, target};

  // Amend prediction based on branch type
  if (type == BranchType::Unconditional) {
    prediction.isTaken = true;
  } else if (type == BranchType::Return) {
    prediction.isTaken = true;
    // Return branches can use the RAS if an entry is available
    if (ras_.size() > 0) {
      prediction.target = ras_.back();
      // Record top of RAS used for target prediction
      rasHistory_[address] = ras_.back();
      ras_.pop_back();
    }
  } else if (type == BranchType::SubroutineCall) {
    prediction.isTaken = true;
    // Subroutine call branches must push their associated return address to RAS
    if (ras_.size() >= rasSize_) {
      ras_.pop_front();
    }
    ras_.push_back(address + 4);
    // Record that this address is a branch-and-link instruction
    rasHistory_[address] = 0;
  } else if (type == BranchType::Conditional) {
    if (!prediction.isTaken) prediction.target = address + 4;
  }

  // Store the global history for correct hashing in update() --
  // needs to be global history and not the hashed index as hashing loses
  // information at longer global history lengths
  FTQ_.emplace_back(prediction.isTaken, globalHistory_);

  // speculatively update global history
  globalHistory_ =
      ((globalHistory_ << 1) | prediction.isTaken) & globalHistoryMask_;

  return prediction;
}

void PerceptronPredictor::update(uint64_t address, bool taken,
                                 uint64_t targetAddress, BranchType type,
                                 uint64_t instructionId) {
  // Get previous branch state and prediction from FTQ
  bool prevPrediction = FTQ_.front().first;
  uint64_t prevGlobalHistory = FTQ_.front().second;
  FTQ_.pop_front();

  // Work out hashed index
  uint64_t hashedIndex =
      ((address >> 2) ^ prevGlobalHistory) & ((1 << btbBits_) - 1);

  std::vector<int8_t> perceptron = btb_[hashedIndex].first;

  // Work out the most recent prediction
  int64_t Pout = getDotProduct(perceptron, prevGlobalHistory);
  bool directionPrediction = (Pout >= 0);

  // Update the perceptron if the prediction was wrong, or the dot product's
  // magnitude was not greater than the training threshold
  if ((directionPrediction != taken) || (static_cast<uint64_t>(std::abs(Pout)) < trainingThreshold_)) {
    int8_t t = (taken) ? 1 : -1;

    for (uint64_t i = 0; i < globalHistoryLength_; i++) {
      int8_t xi =
          ((prevGlobalHistory & (1ULL << ((globalHistoryLength_ - 1) - i))) == 0)
              ? -1
              : 1;
      int8_t product_xi_t = xi * t;
      // Make sure no overflow (+-127)
      if (!(perceptron[i] == 127 && product_xi_t == 1) &&
          !(perceptron[i] == -127 && product_xi_t == -1)) {
        perceptron[i] += product_xi_t;
      }
    }
    perceptron[globalHistoryLength_] += t;
  }

  btb_[hashedIndex].first = perceptron;
  btb_[hashedIndex].second = targetAddress;

  // Update global history if prediction was incorrect.
  // Bit-flip the global history bit corresponding to this prediction; offset
  // = number of younger predictions still in flight (FTQ size). Three
  // correctness guards over the original `globalHistory_ ^= (1 << FTQ_.size())`:
  //   1. `1` is `int`; when FTQ_.size() >= 32 the shift is UB. Use 1ULL and a
  //      hard `< 64` guard so the shift itself is always defined.
  //   2/3. (THE 3rd-bug fix, 2026-05-31) predict()/addToFTQ()/flush() keep
  //      globalHistory_ confined to globalHistoryMask_, but this XOR was the one
  //      site that wrote globalHistory_ WITHOUT re-masking. When the corrected
  //      bit position (FTQ_.size()) is one the mask drops, two things go wrong
  //      together: (a) this branch's contribution has already been masked out of
  //      the live history window, so the correction is semantically moot; and
  //      (b) far worse, the flipped bit is set OUTSIDE the mask, and predict()
  //      only re-masks AFTER its left shift — so the stray bit survives, shifts
  //      into a masked (live) position next cycle, and corrupts the effective
  //      history. That leak is gated on the FTQ_.size() distribution, which is
  //      set by in-flight branch density, which is set by load latency. Hence
  //      lowering LSQ-L1 Access-Latency 4→3 ballooned Dhrystone retired-missrate
  //      10.6%→25% while leaving CoreMark (different density) almost untouched —
  //      the workload-divergent, non-predictor-parameter signature flagged in
  //      perceptron-load-latency-desync-2026-05-29. Only flip when the bit is
  //      actually live in the masked history; then the result stays within mask
  //      automatically (no stray-bit leak).
  if (prevPrediction != taken && FTQ_.size() < 64) {
    const uint64_t correctionBit = 1ULL << FTQ_.size();
    if (globalHistoryMask_ & correctionBit) globalHistory_ ^= correctionBit;
  }
}

void PerceptronPredictor::flush(uint64_t address) {
  // If address interacted with RAS, rewind entry
  auto it = rasHistory_.find(address);
  if (it != rasHistory_.end()) {
    uint64_t target = it->second;
    if (target != 0) {
      // If history entry belongs to a return instruction, push target back onto
      // stack
      if (ras_.size() >= rasSize_) {
        ras_.pop_front();
      }
      ras_.push_back(target);
    } else {
      // If history entry belongs to a branch-and-link instruction, pop target
      // off of stack
      if (ras_.size()) {
        ras_.pop_back();
      }
    }
    rasHistory_.erase(it);
  }

  // Restore pre-predict GHR from the FTQ entry, then pop. The previous
  // `globalHistory_ >>= 1` was wrong: predict() does
  // `(GHR << 1 | bit) & globalHistoryMask_`, which masks off the MSB; flush's
  // right-shift restores bit 0 to zero but cannot recover the dropped MSB,
  // permanently zeroing the oldest history bit on every flush. The desync
  // amplifies under faster mispredict resolution (more flushes/sec), surfacing
  // as Dhrystone branch.missrate 10.6%→25% when LSQ-L1 Access-Latency was
  // lowered 4→3. Save-and-restore is unambiguous.
  if (!FTQ_.empty()) {
    globalHistory_ = FTQ_.back().second;
    FTQ_.pop_back();
  }
}

void PerceptronPredictor::addToFTQ(uint64_t address, bool taken) {
  // Add instruction to the FTQ in event of reused prediction
  FTQ_.emplace_back(taken, globalHistory_);
  globalHistory_ = ((globalHistory_ << 1) | taken) & globalHistoryMask_;
}

int64_t PerceptronPredictor::getDotProduct(
    const std::vector<int8_t>& perceptron, uint64_t history) {
  int64_t Pout = perceptron[globalHistoryLength_];
  for (uint64_t i = 0; i < globalHistoryLength_; i++) {
    // Get branch direction for ith entry in the history
    bool historyTaken =
        ((history & (1ULL << ((globalHistoryLength_ - 1) - i))) != 0);
    Pout += historyTaken ? perceptron[i] : (0 - perceptron[i]);
  }
  return Pout;
}

}  // namespace simeng
