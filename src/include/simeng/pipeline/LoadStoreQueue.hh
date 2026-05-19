#pragma once

#include <deque>
#include <functional>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "simeng/Instruction.hh"
#include "simeng/memory/MemoryInterface.hh"
#include "simeng/pipeline/PipelineBuffer.hh"

namespace simeng {
namespace pipeline {

/** The memory access types which are processed. */
enum accessType { LOAD = 0, STORE };

/** A requestQueue_ entry. */
struct requestEntry {
  /** The memory address(es) to be accessed. */
  std::queue<simeng::memory::MemoryAccessTarget> reqAddresses;
  /** The instruction sending the request(s). */
  std::shared_ptr<Instruction> insn;
};

/** A load store queue (known as "load/store buffers" or "memory order buffer").
 * Holds in-flight memory access requests to ensure load/store consistency. */
class LoadStoreQueue {
 public:
  /** Constructs a combined load/store queue model, simulating a shared queue
   * for both load and store instructions, supplying completion slots for loads
   * and an operand forwarding handler. */
  LoadStoreQueue(
      unsigned int maxCombinedSpace, memory::MemoryInterface& memory,
      span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
      std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
      std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
      bool exclusive = false, uint16_t loadBandwidth = UINT16_MAX,
      uint16_t storeBandwidth = UINT16_MAX,
      uint16_t permittedRequests = UINT16_MAX,
      uint16_t permittedLoads = UINT16_MAX,
      uint16_t permittedStores = UINT16_MAX);

  /** Constructs a split load/store queue model, simulating discrete queues for
   * load and store instructions, supplying completion slots for loads and an
   * operand forwarding handler. */
  LoadStoreQueue(
      unsigned int maxLoadQueueSpace, unsigned int maxStoreQueueSpace,
      memory::MemoryInterface& memory,
      span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
      std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
      std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
      bool exclusive = false, uint16_t loadBandwidth = UINT16_MAX,
      uint16_t storeBandwidth = UINT16_MAX,
      uint16_t permittedRequests = UINT16_MAX,
      uint16_t permittedLoads = UINT16_MAX,
      uint16_t permittedStores = UINT16_MAX);

  /** Retrieve the available space for load uops. For combined queue this is the
   * total remaining space. */
  unsigned int getLoadQueueSpace() const;

  /** Retrieve the available space for store uops. For a combined queue this is
   * the total remaining space. */
  unsigned int getStoreQueueSpace() const;

  /** Retrieve the available space for any memory uops. For a split queue this
   * is the sum of the space in both queues. */
  unsigned int getTotalSpace() const;

  /** Add a load uop to the queue. */
  void addLoad(const std::shared_ptr<Instruction>& insn);

  /** Add a store uop to the queue. */
  void addStore(const std::shared_ptr<Instruction>& insn);

  /** Add the load instruction's memory requests to the requestQueue_. A load
   * whose instruction address has previously caused a memory-order violation
   * (memory-dependence prediction) is held until it can no longer alias an
   * older in-flight store; otherwise it is issued immediately. */
  void startLoad(const std::shared_ptr<Instruction>& insn);

  /** Perform conflict detection and queue the load's memory requests. */
  void issueLoad(const std::shared_ptr<Instruction>& insn);

  /** True if `load` still has a memory-ordering hazard against an older store
   * in the store queue: an older store with an unresolved address, or an
   * older store whose resolved address overlaps the load but is not exactly
   * store-to-load forwardable (wider load / partial overlap). */
  bool olderStoreHazard(const std::shared_ptr<Instruction>& load) const;

  /** True if load `loadSeqId` is currently held in conflictionMap_ for an
   * address overlapping `loadReq`. Such a load is awaiting store-to-load
   * forwarding and never issued a memory read for that address, so it cannot
   * be a memory-order violation against a committing store. */
  bool loadHeldOnAddr(uint64_t loadSeqId,
                      const memory::MemoryAccessTarget& loadReq) const;

  /** Supply the data to be stored by a store operation. */
  void supplyStoreData(const std::shared_ptr<Instruction>& insn);

  /** Commit and write the oldest store instruction to memory, removing it from
   * the store queue. Returns `true` if memory disambiguation has discovered a
   * memory order violation during the commit. */
  bool commitStore(const std::shared_ptr<Instruction>& uop);

  /** Remove the oldest load instruction from the load queue. */
  void commitLoad(const std::shared_ptr<Instruction>& uop);

  /** Remove all flushed instructions from the queues. */
  void purgeFlushed();

  /** Whether this is a combined load/store queue. */
  bool isCombined() const;

  /** Process received load data and send any completed loads for writeback. */
  void tick();

  /** Retrieve the load instruction associated with the most recently discovered
   * memory order violation. */
  std::shared_ptr<Instruction> getViolatingLoad() const;

  /** Diagnostic snapshot of load-latency / memory-level-parallelism counters,
   * emitted as stat lines when SIMENG_LSQ_PROFILE is set. Pure observation,
   * no behavioural effect. */
  std::vector<std::pair<std::string, std::string>> getLsqProfile() const;

 private:
  /** Record a load completing (memory return or store-to-load forward):
   * bucket its issue->complete latency and update in-flight accounting.
   * No-op when lsqProfile_ is false. */
  void lpRecordComplete_(const std::shared_ptr<Instruction>& load,
                         bool viaForward);

  /** The load queue: holds in-flight load instructions. */
  std::deque<std::shared_ptr<Instruction>> loadQueue_;

  /** The store queue: holds in-flight store instructions with its associated
   * data. */
  std::deque<std::pair<std::shared_ptr<Instruction>,
                       span<const simeng::RegisterValue>>>
      storeQueue_;

  /** Slots to write completed load instructions into for writeback. */
  span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots_;

  /** Map of loads that have requested their data, keyed by sequence ID. */
  std::unordered_map<uint64_t, std::shared_ptr<Instruction>> requestedLoads_;

  /** A function handler to call to forward the results of a completed load. */
  std::function<void(span<Register>, span<RegisterValue>)> forwardOperands_;

  /** A function handle called upon exception generation. */
  std::function<void(const std::shared_ptr<Instruction>&)> raiseException_;

  /** The maximum number of loads that can be in-flight. Undefined if this
   * is a combined queue. */
  unsigned int maxLoadQueueSpace_;

  /** The maximum number of stores that can be in-flight. Undefined if this is a
   * combined queue. */
  unsigned int maxStoreQueueSpace_;

  /** The maximum number of memory ops that can be in-flight. Undefined if this
   * is a split queue. */
  unsigned int maxCombinedSpace_;

  /** Whether this queue is combined or split. */
  bool combined_;

  /** Retrieve the load queue space for a split queue. */
  unsigned int getLoadQueueSplitSpace() const;

  /** Retrieve the store queue space for a split queue. */
  unsigned int getStoreQueueSplitSpace() const;

  /** Retrieve the total memory uop space available for a combined queue. */
  unsigned int getCombinedSpace() const;

  /** A pointer to process memory. */
  memory::MemoryInterface& memory_;

  /** The load instruction associated with the most recently discovered memory
   * order violation. */
  std::shared_ptr<Instruction> violatingLoad_ = nullptr;

  /** The number of times this unit has been ticked. */
  uint64_t tickCounter_ = 0;

  /** Memory-dependence predictor: instruction addresses of loads that have
   * caused a memory-order violation. Such loads are conservatively held (see
   * startLoad) on subsequent executions until they can no longer alias an
   * older in-flight store, preventing an unbounded violation/flush storm on
   * RAW-through-memory hazards in tight loops, while a load's first execution
   * still issues speculatively (preserving baseline violation behaviour). */
  std::unordered_set<uint64_t> memDepViolatorPCs_;

  /** Loads held by the memory-dependence predictor, re-evaluated each tick()
   * and released to issueLoad() once they no longer hazard an older store. */
  std::vector<std::shared_ptr<Instruction>> pendingDisambiguation_;

  /** A map to hold load instructions that are stalled due to a detected
   * memory reordering confliction. First key is a store's sequence id and the
   * second key the conflicting address. The value takes the form of a vector of
   * pairs containing a pointer to the conflicted load and the size of the data
   * needed at that address by the load. */
  std::unordered_map<
      uint64_t,
      std::unordered_map<
          uint64_t,
          std::vector<std::pair<std::shared_ptr<Instruction>, uint16_t>>>>
      conflictionMap_;

  /** A map between LSQ cycles and load requests ready on that cycle. */
  std::map<uint64_t, std::deque<requestEntry>> requestLoadQueue_;

  /** A map between LSQ cycles and store requests ready on that cycle. */
  std::map<uint64_t, std::deque<requestEntry>> requestStoreQueue_;

  /** A queue of completed loads ready for writeback. */
  std::queue<std::shared_ptr<Instruction>> completedLoads_;

  /** Whether the LSQ can only process loads xor stores within a cycle. */
  bool exclusive_;

  /** The amount of data readable from the L1D cache per cycle. */
  uint16_t loadBandwidth_;

  /** The amount of data writable to the L1D cache per cycle. */
  uint16_t storeBandwidth_;

  /** The combined limit of loads and store requests permitted per cycle. */
  uint16_t totalLimit_;

  /** The number of loads and stores permitted per cycle. */
  std::array<uint16_t, 2> reqLimits_;

  /** --- Diagnostic counters (SIMENG_LSQ_PROFILE). Pure observation; gated by
   * lsqProfile_ so cost is near-zero when disabled. --- */
  /** Whether load-latency profiling is enabled (env-set, cached in ctor). */
  bool lsqProfile_ = false;
  /** Loads that went through the memory-request path (had addresses). */
  uint64_t lpLoadsIssued_ = 0;
  /** Loads that early-executed with no addresses (zero memory latency). */
  uint64_t lpLoadsEarly_ = 0;
  /** Loads completed by a memory return. */
  uint64_t lpLoadsViaMem_ = 0;
  /** Loads completed by store-to-load forwarding (conflictionMap_). */
  uint64_t lpLoadsViaFwd_ = 0;
  /** Loads issued but not yet completed (memory-level parallelism gauge). */
  int64_t lpInFlight_ = 0;
  /** Sum of lpInFlight_ sampled once per tick (for average MLP). */
  uint64_t lpOutstandingSum_ = 0;
  /** Number of per-tick samples taken (ticks with >=1 load in flight). */
  uint64_t lpOutstandingSamples_ = 0;
  /** Peak simultaneous in-flight loads. */
  uint64_t lpOutstandingMax_ = 0;
  /** Sum of every completed load's issue->complete latency (for average). */
  uint64_t lpLatSum_ = 0;
  /** Issue->complete latency histogram: 0, 1-2, 3-4, 5-8, 9-16, 17-32,
   * 33-64, 65+ cycles. */
  std::array<uint64_t, 8> lpLatBucket_ = {0, 0, 0, 0, 0, 0, 0, 0};
  /** Per-load issue tick (seqId -> tickCounter_ at issue), profiling only. */
  std::unordered_map<uint64_t, uint64_t> lpIssueTick_;
};

}  // namespace pipeline
}  // namespace simeng
