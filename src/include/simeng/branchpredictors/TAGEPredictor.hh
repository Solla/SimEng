#pragma once

#include <algorithm>
#include <cassert>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "simeng/branchpredictors/BranchHistory.hh"
#include "simeng/branchpredictors/BranchPredictor.hh"
#include "simeng/config/SimInfo.hh"

namespace simeng {

/** A data structure to store all of the information needed for a single entry
 * in a tagged table. */
struct TAGEEntry {
  uint8_t satCnt;
  uint64_t tag;
  uint8_t u;  // usefulness counter
  uint64_t target;
};

/** A data structure to store all of the information needed for a single entry
 * in the Fetch Target Queue. */
struct ftqEntry {
  int8_t predTable;
  std::shared_ptr<uint64_t[]> indices;
  std::shared_ptr<uint64_t[]> tags;
  BranchPrediction prediction;
  BranchPrediction altPrediction;
};

/**
 * A TAGE branch predictor of the type described by Seznec and Michaud:
 * https://inria.hal.science/hal-03408381/document.  A brief summary of the
 * prediction mechanism is described below.
 *
 * This predictor uses a series of prediction tables (a user-defined number
 * thereof), each of which uses a progressively larger global history to index
 * it. The default prediction table does not use any global history.
 *
 * To access a prediction table, an XOR hash of the branch's address and the
 * global history of the relevant length is used to index the table.  Then, a
 * tag is determined by a hash of the address and the context of the branch is
 * used to confirm that the entry belongs to the present branch.
 *
 * A prediction is made on the basis of the prediction table using the longest
 * global history that has an entry corresponding to the present branch
 * (matching tag).
 * */

class TAGEPredictor : public BranchPredictor {
 public:
  /** Initialise predictor models. */
  TAGEPredictor(ryml::ConstNodeRef config = config::SimInfo::getConfig());

  ~TAGEPredictor();

  /** Generate a branch prediction for the supplied instruction address, a
   * branch type, and a known branch offset.  Returns a branch direction and
   * branch target address. */
  BranchPrediction predict(uint64_t address, BranchType type,
                           int64_t knownOffset) override;

  /** Updates appropriate predictor model objects based on the address, type and
   * outcome of the branch instruction.  Update must be called on
   * branches in program order.  To check this, instructionId is also passed
   * to this function. */
  void update(uint64_t address, bool isTaken, uint64_t targetAddress,
              BranchType type, uint64_t instructionId) override;

  /** Provides flushing behaviour for the implemented branch prediction schemes
   * via the instruction address.  Branches must be flushed in reverse
   * program order (though, if a block of n instructions is being flushed at
   * once, the exact order that the individual instructions within this block
   * are flushed does not matter so long as they are all flushed). */
  void flush(uint64_t address) override;

 private:
  /** Returns a prediction for a branch at this address from the non-tagged BTB
   * that is used for default predictions. */
  BranchPrediction getBtbPrediction(uint64_t address);

  /** Provides a prediction, alternative prediction, the table number that
   * provided the prediction, and the indices and tags of the prediction and
   * alternative prediction.  This prediction info is determined from the
   * tagged tables for a branch with the provided address. */
  void getTaggedPrediction(uint64_t address, BranchPrediction* prediction,
                           BranchPrediction* altPrediction, int8_t* predTable,
                           std::shared_ptr<uint64_t[]> indices,
                           std::shared_ptr<uint64_t[]> tags);

  /** Returns the index of a branch in a tagged table for a given address and
   * table. */
  uint64_t getTaggedIndex(uint64_t address, uint8_t table);

  /** Returns a hash of the address and the global history that is then trimmed
   * to the appropriate tag length.  The tag varies depending on the table
   * that is being accessed. */
  uint64_t getTag(uint64_t address, uint8_t table);

  /** Updates the default, untagged prediction table on the basis of the
   * outcome of a branch. */
  void updateBtb(uint64_t address, bool isTaken, uint64_t target);

  /** Updates the tagged tables on the basis of the outcome of a branch. */
  void updateTaggedTables(bool isTaken, uint64_t target);

  /** The bitlength of the BTB (i.e., default prediction table) index; BTB
   * will have 2^bits entries. */
  uint8_t btbBits_;

  /** A 2^bits length vector of pairs containing a satCntBits_-bit saturating
   * counter and a branch target.  This is the untagged, default prediction
   * table. */
  std::vector<std::pair<uint8_t, uint64_t>> btb_;

  /** The bitlength of the Tagged tables' indices.
   * Each tagged table will have 2^bits entries. */
  uint8_t TAGETableBits_;

  /** The number of tagged tables in the TAGE scheme.
   * In addition to the tagged tables, there will be a single untagged table
   * (the BTB) from which default predictions will be made. */
  uint8_t numTAGETables_;

  /** Data structure to store the tagged tables in. */
  std::vector<std::vector<TAGEEntry>> TAGETables_;

  /** Fetch Target Queue containing the direction prediction and previous global
   * history state of branches that are currently unresolved */
  std::deque<ftqEntry> ftq_;

  /** The number of bits used to form the saturating counter in a BTB entry. */
  uint8_t satCntBits_;

  /** The number of previous branch directions recorded globally. */
  uint16_t globalHistoryLength_;

  /** A return address stack. */
  std::deque<uint64_t> ras_;

  /** RAS history with instruction address as the keys. A non-zero value
   * represents the target prediction for a return instruction and a 0 entry for
   * a branch-and-link instruction. */
  std::map<uint64_t, uint64_t> rasHistory_;

  /** The size of the RAS.  I.e., the maximum capacity of the RAS. */
  uint16_t rasSize_;

  /** An n-bit history of previous branch directions where n is equal to
   * globalHistoryLength_.  Each bit represents a branch taken (1) or not
   * taken (0), with the most recent branch being the least-significant-bit */
  BranchHistory globalHistory_;

  /** The size of the tags used in the tagged tables, where the units of
   * size are bits. */
  uint8_t tagLength_;

  /** Diagnostic counters (env-gated via SIMENG_TAGE_PROFILE). Track the
   * addHistory / rollBack / update balance to detect global-history
   * imbalance under high flush rate. */
  bool tageProfileEnabled_ = false;
  uint64_t predictCount_ = 0;
  uint64_t addHistoryCount_ = 0;
  uint64_t flushCount_ = 0;
  uint64_t rollBackCount_ = 0;
  uint64_t updateCount_ = 0;
  uint64_t updateHistoryFlipCount_ = 0;
  // Provider histogram: providerHits_[i+1] counts predictions from table i,
  // providerHits_[0] counts BTB-only (no tagged-table hit). Size 8 covers
  // BTB + up to 7 tagged tables. providerMisses_ mirrors structure.
  uint64_t providerHits_[8] = {0,0,0,0,0,0,0,0};
  uint64_t providerMisses_[8] = {0,0,0,0,0,0,0,0};
  // Periodic u-counter aging counter. Standard TAGE decays all u-counters
  // every ~256K updates so saturated-useful entries don't lock out new
  // allocations. Without this, allocation rate collapses over time and
  // ~30% of branches end up with no tagged-table hit at steady state.
  uint64_t updatesSinceUAging_ = 0;
  uint64_t uAgingEvents_ = 0;

 public:
  /** Diagnostic accessors for SIMENG_TAGE_PROFILE bench. */
  uint64_t getPredictCount() const { return predictCount_; }
  uint64_t getAddHistoryCount() const { return addHistoryCount_; }
  uint64_t getFlushCount() const { return flushCount_; }
  uint64_t getRollBackCount() const { return rollBackCount_; }
  uint64_t getUpdateCount() const { return updateCount_; }
  uint64_t getUpdateHistoryFlipCount() const { return updateHistoryFlipCount_; }
  uint64_t getFtqSize() const { return ftq_.size(); }

  std::map<std::string, uint64_t> getDiagnostics() const override {
    if (!tageProfileEnabled_) return {};
    return {
        {"tage.predict", predictCount_},
        {"tage.addHistory", addHistoryCount_},
        {"tage.flush", flushCount_},
        {"tage.rollBack", rollBackCount_},
        {"tage.update", updateCount_},
        {"tage.updateHistoryFlip", updateHistoryFlipCount_},
        {"tage.ftqResidual", static_cast<uint64_t>(ftq_.size())},
        {"tage.provider.BTB.hits", providerHits_[0]},
        {"tage.provider.BTB.miss", providerMisses_[0]},
        {"tage.provider.T0.hits", providerHits_[1]},
        {"tage.provider.T0.miss", providerMisses_[1]},
        {"tage.provider.T1.hits", providerHits_[2]},
        {"tage.provider.T1.miss", providerMisses_[2]},
        {"tage.provider.T2.hits", providerHits_[3]},
        {"tage.provider.T2.miss", providerMisses_[3]},
        {"tage.provider.T3.hits", providerHits_[4]},
        {"tage.provider.T3.miss", providerMisses_[4]},
        {"tage.provider.T4.hits", providerHits_[5]},
        {"tage.provider.T4.miss", providerMisses_[5]},
        {"tage.provider.T5.hits", providerHits_[6]},
        {"tage.provider.T5.miss", providerMisses_[6]},
        {"tage.uAgingEvents", uAgingEvents_},
        // Expected: ftq.size() == addHistory - rollBack - update.
        // Any divergence indicates a leak in the predict/flush/commit
        // accounting that corrupts global history state.
    };
  }

 private:
  // This variable is used only in debug mode -- therefore hide behind ifdef
#ifndef NDEBUG
  /** The Id of the last instruction that update was called on -- used to
   * ensure that update is called in program order. */
  uint64_t lastUpdatedInstructionId = 0;
#endif
};

}  // namespace simeng
