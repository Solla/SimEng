#pragma once

#include <array>
#include <functional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "simeng/MemoryInterface.hh"
#include "simeng/arch/Architecture.hh"
#include "simeng/pipeline/PipelineBuffer.hh"

namespace simeng {
namespace pipeline {

/** The various states of the loop buffer. */
enum class LoopBufferState {
  IDLE = 0,  // No operations
  WAITING,   // Waiting to find boundary instruction in fetch stream
  FILLING,   // Filling loop buffer with loop body
  SUPPLYING  // Feeding loop buffer content to output buffer
};

// Struct to hold information about a fetched instruction
struct loopBufferEntry {
  // Encoding of the instruction
  const uint64_t encoding;

  // Size of the instruction
  const uint16_t instructionSize;

  // PC of the instruction
  const uint64_t address;

  // Branch prediction made for instruction
  const BranchPrediction prediction;
};

/** A fetch and pre-decode unit for a pipelined processor. Responsible for
 * reading instruction memory and maintaining the program counter. */
class FetchUnit {
 public:
  /** Construct a fetch unit with a reference to an output buffer, the ISA, and
   * the current branch predictor, and information on the instruction memory. */
  FetchUnit(PipelineBuffer<MacroOp>& output, MemoryInterface& instructionMemory,
            uint8_t blockSize, const arch::Architecture& isa,
            BranchPredictor& branchPredictor);

  ~FetchUnit();

  /** Tick the fetch unit. Retrieves and pre-decodes the instruction at the
   * current program counter. */
  void tick();

  /** Function handle to retrieve branch that represents loop boundary. */
  void registerLoopBoundary(uint64_t branchAddress);

  /** Check whether the program has ended. Returns `true` if the current PC is
   * outside of instruction memory. */
  bool hasHalted() const;

  /** Update the program counter to the specified address.
   * NOTE: Must set program length before calling when scheduling.
   */
  void updatePC(uint64_t address);

  /** Update programByteLength_ to the specified value.
   * NOTE: Must be set before updating PC when scheduling.
   */
  void setProgramLength(uint64_t size);

  /** Request instructions at the current program counter for a future cycle. */
  void requestFromPC();

  /** Retrieve the number of cycles fetch terminated early due to a predicted
   * branch. */
  uint64_t getBranchStalls() const;

  /** Diagnostic: characterise WHERE taken-branch fetch bubbles and
   * loop-buffer (non-)engagement originate. Returned as ordered
   * {key,value} pairs; emitted into Core stats only when the
   * SIMENG_FETCH_PROFILE env var is set. Pure observation — no effect
   * on simulated behaviour. */
  std::vector<std::pair<std::string, std::string>> getFetchProfile() const;

  /** Clear the loop buffer. */
  void flushLoopBuffer();

  /** Install a callback invoked whenever the loop buffer disengages
   * (loop exit or #4c abort), used to re-arm ROB loop detection so the
   * next loop can be detected. */
  void setOnLoopBufferIdle(std::function<void()> fn) {
    onLoopBufferIdle_ = std::move(fn);
  }

  /** Temporarily pause the FetchUnit. */
  void pause() {
    paused_ = true;
    instructionMemory_.clearCompletedReads();
    flushLoopBuffer();
  };

  /** Unpause the fetch unit. */
  void unpause() {
    paused_ = false;
    requestFromPC();
  };

  /** Get the current PC value. */
  uint64_t getPC() const { return pc_; };

 private:
  /** An output buffer connecting this unit to the decode unit. */
  PipelineBuffer<MacroOp>& output_;

  /** The current program counter. */
  uint64_t pc_ = 0;

  /** An interface to the instruction memory. */
  MemoryInterface& instructionMemory_;

  /** The length of the available instruction memory. */
  uint64_t programByteLength_ = 0;

  /** Reference to the currently used ISA. */
  const arch::Architecture& isa_;

  /** Reference to the current branch predictor. */
  BranchPredictor& branchPredictor_;

  /** A loop buffer to supply a detected loop instruction stream. */
  std::deque<loopBufferEntry> loopBuffer_;

  /** State of the loop buffer. */
  LoopBufferState loopBufferState_ = LoopBufferState::IDLE;

  /** The branch instruction that forms the loop. */
  uint64_t loopBoundaryAddress_ = 0;

  /** The current program halt state. Set to `true` when the PC leaves the
   * instruction memory region, and set back to `false` if the PC is returned to
   * the instruction region. */
  bool hasHalted_ = false;

  /** Callback to re-arm ROB loop detection when the loop buffer
   * disengages. Empty until wired by the Core. */
  std::function<void()> onLoopBufferIdle_;

  /** The number of cycles fetch terminated early due to a predicted branch. */
  uint64_t branchStalls_ = 0;

  /** --- Diagnostic counters (SIMENG_FETCH_PROFILE). Pure observation. ---
   * BranchType has 6 enumerators (Conditional..Unknown). */
  /** Predicted-taken branches that ended a fetch group, by branch type. */
  std::array<uint64_t, 6> takenByType_ = {};
  /** Subset of the above that also wasted >=1 fetch slot (a branchStall),
   * by branch type. */
  std::array<uint64_t, 6> stallByType_ = {};
  /** Total fetch slots wasted by taken-branch truncation (sum of the
   * empty trailing slots, not just the stall-cycle count). */
  uint64_t branchStallSlots_ = 0;
  /** Loop-buffer engagement: FILLING entered (WAITING->FILLING). */
  uint64_t lbFillStarted_ = 0;
  /** FILLING aborted because the body had a non-boundary branch (#4c). */
  uint64_t lbFillAbortedBranch_ = 0;
  /** FILLING aborted because the boundary branch was predicted to exit. */
  uint64_t lbFillAbortedExit_ = 0;
  /** SUPPLYING entered (a full single-BB loop body was captured). */
  uint64_t lbSupplyEntered_ = 0;
  /** Instructions actually supplied from the loop buffer (no re-fetch). */
  uint64_t lbSupplyInstrs_ = 0;

  /** The size of a fetch block, in bytes. */
  uint8_t blockSize_;

  /** A mask of the bits of the program counter to use for obtaining the block
   * address to fetch. */
  uint64_t blockMask_;

  /** The buffer used to hold fetched instruction data. */
  uint8_t* fetchBuffer_;

  /** The amount of data currently in the fetch buffer. */
  uint8_t bufferedBytes_ = 0;

  /** The Fetch Unit's paused state - when an interupt has been signalled, the
   * Fetch Unit must not fetch / increment the PC until a new process has been
   * scheduled. This ensures the correct architectural state can be captured
   * during a context switch. */
  bool paused_ = false;
};

}  // namespace pipeline
}  // namespace simeng
