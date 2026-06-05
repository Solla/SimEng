#include "simeng/models/outoforder/Core.hh"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>

// Temporary; until config options are available
#include "simeng/arch/aarch64/Instruction.hh"
namespace simeng {
namespace models {
namespace outoforder {

// TODO: System register count has to match number of supported system registers
Core::Core(MemoryInterface& instructionMemory, MemoryInterface& dataMemory,
           const arch::Architecture& isa, BranchPredictor& branchPredictor,
           std::shared_ptr<memory::MMU> mmu,
           pipeline::PortAllocator& portAllocator,
           arch::sendSyscallToHandler handleSyscall, YAML::Node& config)
    : isa_(isa),
      branchPredictor_(branchPredictor),
      physicalRegisterStructures_(isa.getConfigPhysicalRegisterStructure()),
      physicalRegisterQuantities_(isa.getConfigPhysicalRegisterQuantities()),
      registerFileSet_(physicalRegisterStructures_),
      registerAliasTable_(isa.getRegisterFileStructures(),
                          physicalRegisterQuantities_),
      mappedRegisterFileSet_(registerFileSet_, registerAliasTable_),
      dataMemory_(dataMemory),
      mmu_(mmu),
      fetchToDecodeBuffer_(
          config["Pipeline-Widths"]["FrontEnd"].as<unsigned int>(), {}),
      decodeToRenameBuffer_(
          config["Pipeline-Widths"]["FrontEnd"].as<unsigned int>(), nullptr),
      renameToDispatchBuffer_(
          config["Pipeline-Widths"]["FrontEnd"].as<unsigned int>(), nullptr),
      issuePorts_(config["Execution-Units"].size(), {1, nullptr}),
      completionSlots_(
          config["Execution-Units"].size() +
              config["Pipeline-Widths"]["LSQ-Completion"].as<unsigned int>(),
          {1, nullptr}),
      loadStoreQueue_(
          config["Queue-Sizes"]["Load"].as<unsigned int>(),
          config["Queue-Sizes"]["Store"].as<unsigned int>(), dataMemory,
          {completionSlots_.data() + config["Execution-Units"].size(),
           config["Pipeline-Widths"]["LSQ-Completion"].as<unsigned int>()},
          [this](auto regs, auto values) {
            dispatchIssueUnit_.forwardOperands(regs, values);
          },
          [this](auto instruction) { raiseException(instruction); },
          config["LSQ-L1-Interface"]["Exclusive"].as<bool>(),
          config["LSQ-L1-Interface"]["Load-Bandwidth"].as<uint16_t>(),
          config["LSQ-L1-Interface"]["Store-Bandwidth"].as<uint16_t>(),
          config["LSQ-L1-Interface"]["Permitted-Requests-Per-Cycle"]
              .as<uint16_t>(),
          config["LSQ-L1-Interface"]["Permitted-Loads-Per-Cycle"]
              .as<uint16_t>(),
          config["LSQ-L1-Interface"]["Permitted-Stores-Per-Cycle"]
              .as<uint16_t>(),
          config["LSQ-L1-Interface"]["Load-To-Load-Forwarding"].as<bool>(),
          config["LSQ-L1-Interface"]["Load-To-Load-Forward-Latency"]
              .as<uint64_t>()),
      fetchUnit_(fetchToDecodeBuffer_, instructionMemory,
                 config["Fetch"]["Fetch-Block-Size"].as<uint16_t>(), isa,
                 branchPredictor),
      reorderBuffer_(
          config["Queue-Sizes"]["ROB"].as<unsigned int>(), registerAliasTable_,
          loadStoreQueue_,
          [this](auto instruction) { raiseException(instruction); },
          [this](auto branchAddress) {
            fetchUnit_.registerLoopBoundary(branchAddress);
          },
          branchPredictor, config["Fetch"]["Loop-Buffer-Size"].as<uint16_t>(),
          config["Fetch"]["Loop-Detection-Threshold"].as<uint16_t>()),
      decodeUnit_(fetchToDecodeBuffer_, decodeToRenameBuffer_, branchPredictor),
      renameUnit_(decodeToRenameBuffer_, renameToDispatchBuffer_,
                  reorderBuffer_, registerAliasTable_, loadStoreQueue_,
                  physicalRegisterStructures_.size()),
      dispatchIssueUnit_(renameToDispatchBuffer_, issuePorts_, registerFileSet_,
                         portAllocator, physicalRegisterQuantities_),
      writebackUnit_(
          completionSlots_, registerFileSet_,
          [this](auto insnId) { reorderBuffer_.commitMicroOps(insnId); }),
      portAllocator_(portAllocator),
      commitWidth_(config["Pipeline-Widths"]["Commit"].as<unsigned int>()),
      handleSyscall_(handleSyscall) {
  for (size_t i = 0; i < config["Execution-Units"].size(); i++) {
    // Create vector of blocking groups
    std::vector<uint16_t> blockingGroups = {};
    if (config["Execution-Units"][i]["Blocking-Groups"].IsDefined()) {
      for (YAML::Node gp : config["Execution-Units"][i]["Blocking-Groups"]) {
        blockingGroups.push_back(gp.as<uint16_t>());
      }
    }
    executionUnits_.emplace_back(
        issuePorts_[i], completionSlots_[i],
        [this](auto regs, auto values) {
          dispatchIssueUnit_.forwardOperands(regs, values);
        },
        [this](auto uop) { loadStoreQueue_.startLoad(uop); },
        [this](auto uop) { loadStoreQueue_.supplyStoreData(uop); },
        [](auto uop) { uop->setCommitReady(); }, branchPredictor,
        config["Execution-Units"][i]["Pipelined"].as<bool>(), blockingGroups);
  }
  // Provide reservation size getter to A64FX port allocator
  portAllocator.setRSSizeGetter([this](std::vector<uint32_t>& sizeVec) {
    dispatchIssueUnit_.getRSSizes(sizeVec);
  });
  // Re-arm ROB loop detection whenever the loop buffer disengages, so the
  // next loop can be detected. Without this, ReorderBuffer::loopDetected_
  // latches on the first detected loop and only clears on a pipeline flush
  // — near-absent in low-mispredict loop-heavy code (e.g. Dhrystone),
  // leaving the loop buffer dormant for the rest of the run.
  fetchUnit_.setOnLoopBufferIdle(
      [this]() { reorderBuffer_.resetLoopDetection(); });
  // Create exception handler based on chosen architecture
  exceptionHandlerFactory(config["Core"]["ISA"].as<std::string>());
}

void Core::tick() {
  ticks_++;
  isa_.updateSystemTimerRegisters(&registerFileSet_, ticks_);

  switch (status_) {
    case CoreStatus::idle:
      idle_ticks_++;
      return;
    case CoreStatus::switching: {
      // Ensure that all pipeline buffers and ROB are empty, no data requests
      // are pending, and no exception is being handled before context switching
      if (fetchToDecodeBuffer_.isEmpty() && decodeToRenameBuffer_.isEmpty() &&
          renameToDispatchBuffer_.isEmpty() &&
          dispatchIssueUnit_.isSideQueueEmpty() &&
          !dataMemory_.hasPendingRequests() && (reorderBuffer_.size() == 0) &&
          (exceptionGenerated_ == false)) {
        // Flush pipeline
        fetchUnit_.flushLoopBuffer();
        decodeUnit_.purgeFlushed();
        dispatchIssueUnit_.purgeFlushed();
        dispatchIssueUnit_.flush();
        writebackUnit_.flush();
        status_ = CoreStatus::idle;
        return;
      }
      break;
    }
    case CoreStatus::halted:
      return;
    case CoreStatus::executing:
      break;
  }

  // Increase tick count for current process execution
  procTicks_++;

  if (exceptionGenerated_) {
    processException();
    return;
  }

  // Tick port allocators internal functionality at start of cycle
  portAllocator_.tick();

  // Writeback must be ticked at start of cycle, to ensure decode reads the
  // correct values
  writebackUnit_.tick();

  // Tick units
  fetchUnit_.tick();
  decodeUnit_.tick();
  renameUnit_.tick();
  dispatchIssueUnit_.tick();
  for (auto& eu : executionUnits_) {
    // Tick each execution unit
    eu.tick();
  }

  loadStoreQueue_.tick();

  // Late tick for the dispatch/issue unit to issue newly ready uops
  dispatchIssueUnit_.issue();

  // Tick buffers
  // Each unit must have wiped the entries at the head of the buffer after
  // use, as these will now loop around and become the tail.
  fetchToDecodeBuffer_.tick();
  decodeToRenameBuffer_.tick();
  renameToDispatchBuffer_.tick();
  for (auto& issuePort : issuePorts_) {
    issuePort.tick();
  }
  for (auto& completionSlot : completionSlots_) {
    completionSlot.tick();
  }

  // Commit instructions from ROB
  reorderBuffer_.commit(commitWidth_);

  if (exceptionGenerated_) {
    handleException();
    fetchUnit_.requestFromPC();
    return;
  }

  flushIfNeeded();
  fetchUnit_.requestFromPC();
}

void Core::flushIfNeeded() {
  // Check for flush
  bool euFlush = false;
  uint64_t targetAddress = 0;
  uint64_t lowestSeqId = 0;
  for (const auto& eu : executionUnits_) {
    if (eu.shouldFlush() && (!euFlush || eu.getFlushSeqId() < lowestSeqId)) {
      euFlush = true;
      lowestSeqId = eu.getFlushSeqId();
      targetAddress = eu.getFlushAddress();
    }
  }
  if (euFlush || reorderBuffer_.shouldFlush()) {
    // Flush was requested in an out-of-order stage.
    // Update PC and wipe in-order buffers (Fetch/Decode, Decode/Rename,
    // Rename/Dispatch)

    if (reorderBuffer_.shouldFlush() &&
        (!euFlush || reorderBuffer_.getFlushInsnId() < lowestSeqId)) {
      // If the reorder buffer found an older instruction to flush up to, do
      // that instead
      lowestSeqId = reorderBuffer_.getFlushInsnId();
      targetAddress = reorderBuffer_.getFlushAddress();
    }

    fetchUnit_.flushLoopBuffer();
    fetchUnit_.updatePC(targetAddress);

    // Rewind the predictor for branches squashed in the front-end buffers
    // before they are wiped (these are younger than any ROB branch, so this
    // must precede reorderBuffer_.flush()).
    flushFrontEndPredictions(true);

    fetchToDecodeBuffer_.fill({});
    fetchToDecodeBuffer_.stall(false);

    decodeToRenameBuffer_.fill(nullptr);
    decodeToRenameBuffer_.stall(false);

    renameToDispatchBuffer_.fill(nullptr);
    renameToDispatchBuffer_.stall(false);

    // Flush everything younger than the bad instruction from the ROB
    reorderBuffer_.flush(lowestSeqId);
    decodeUnit_.purgeFlushed();
    dispatchIssueUnit_.purgeFlushed();
    loadStoreQueue_.purgeFlushed();
    for (auto& eu : executionUnits_) {
      eu.purgeFlushed();
    }

    flushes_++;
  } else if (decodeUnit_.shouldFlush()) {
    // Flush was requested at decode stage
    // Update PC and wipe Fetch/Decode buffer.
    targetAddress = decodeUnit_.getFlushAddress();

    fetchUnit_.flushLoopBuffer();
    fetchUnit_.updatePC(targetAddress);

    // Only the Fetch/Decode buffer is wiped here; rewind the predictor for
    // just those branches.
    flushFrontEndPredictions(false);

    fetchToDecodeBuffer_.fill({});
    fetchToDecodeBuffer_.stall(false);

    flushes_++;
  }
}

void Core::flushFrontEndPredictions(bool includeDecodeRename) {
  // Branches sitting in the Fetch/Decode or Decode/Rename buffers had
  // predict() called (FTQ pushed / global history advanced) but are not yet
  // reserved in the reorder buffer (RenameUnit reserves the ROB entry at the
  // same time it writes renameToDispatchBuffer_, so renameToDispatch branches
  // are already covered by reorderBuffer_.flush() and must NOT be rewound
  // again here). Rewind the not-yet-reserved branches so the predictor's FTQ
  // stays in sync. Only the count of flush() calls matters for history, so
  // iteration order is unimportant.
  auto flushBranch = [this](const std::shared_ptr<Instruction>& insn) {
    if (insn != nullptr && insn->isBranch()) {
      branchPredictor_.flush(insn->getInstructionAddress());
    }
  };

  // Fetch/Decode buffer holds MacroOps (vectors of micro-ops); the branch, if
  // any, is the head micro-op.
  for (auto* slots :
       {fetchToDecodeBuffer_.getHeadSlots(),
        fetchToDecodeBuffer_.getTailSlots()}) {
    for (size_t i = 0; i < fetchToDecodeBuffer_.getWidth(); i++) {
      const MacroOp& macroOp = slots[i];
      if (!macroOp.empty()) flushBranch(macroOp[0]);
    }
  }

  if (!includeDecodeRename) return;

  // NOTE: only decodeToRenameBuffer_ — renameToDispatchBuffer_ branches are
  // already reserved in the ROB and rewound by reorderBuffer_.flush().
  for (auto* slots : {decodeToRenameBuffer_.getHeadSlots(),
                      decodeToRenameBuffer_.getTailSlots()}) {
    for (size_t i = 0; i < decodeToRenameBuffer_.getWidth(); i++) {
      flushBranch(slots[i]);
    }
  }
}

CoreStatus Core::getStatus() { return status_; }

void Core::setStatus(CoreStatus newStatus) { status_ = newStatus; }

uint64_t Core::getCurrentTID() const { return currentTID_; }

uint64_t Core::getCoreId() const { return coreId_; }

void Core::raiseException(const std::shared_ptr<Instruction>& instruction) {
  exceptionGenerated_ = true;
  exceptionGeneratingInstruction_ = instruction;
}

void Core::handleException() {
  fetchToDecodeBuffer_.fill({});
  fetchToDecodeBuffer_.stall(false);

  decodeToRenameBuffer_.fill(nullptr);
  decodeToRenameBuffer_.stall(false);

  renameToDispatchBuffer_.fill(nullptr);
  renameToDispatchBuffer_.stall(false);

  // Flush everything younger than the exception-generating instruction.
  // This must happen prior to handling the exception to ensure the commit
  // state is up-to-date with the register mapping table
  reorderBuffer_.flush(exceptionGeneratingInstruction_->getInstructionId());
  decodeUnit_.purgeFlushed();
  dispatchIssueUnit_.purgeFlushed();
  loadStoreQueue_.purgeFlushed();
  for (auto& eu : executionUnits_) {
    eu.purgeFlushed();
  }

  exceptionHandler_->registerException(exceptionGeneratingInstruction_);
  processException();
}

void Core::processException() {
  assert(exceptionGenerated_ != false &&
         "[SimEng:Core] Attempted to process an exception handler that wasn't "
         "active");
  if (dataMemory_.hasPendingRequests()) {
    // Must wait for all memory requests to complete before processing the
    // exception
    return;
  }

  bool success = exceptionHandler_->tick();
  if (!success) {
    // Exception handler requires further ticks to complete
    return;
  }

  const auto& result = exceptionHandler_->getResult();

  if (result.fatal) {
    status_ = CoreStatus::halted;
    std::cout << "[SimEng:Core] Halting due to fatal exception" << std::endl;
  } else {
    fetchUnit_.flushLoopBuffer();
    fetchUnit_.updatePC(result.instructionAddress);
    applyStateChange(result.stateChange);
    if (result.idleAfterSyscall) {
      // Enusre all pipeline stages are flushed
      dispatchIssueUnit_.flush();
      writebackUnit_.flush();
      // Update core status
      status_ = CoreStatus::idle;
      contextSwitches_++;
    }
  }

  exceptionGenerated_ = false;
}

void Core::applyStateChange(const OS::ProcessStateChange& change) {
  // Update registers in accoradance with the ProcessStateChange type
  switch (change.type) {
    case OS::ChangeType::INCREMENT: {
      for (size_t i = 0; i < change.modifiedRegisters.size(); i++) {
        mappedRegisterFileSet_.set(
            change.modifiedRegisters[i],
            mappedRegisterFileSet_.get(change.modifiedRegisters[i])
                    .get<uint64_t>() +
                change.modifiedRegisterValues[i].get<uint64_t>());
      }
      break;
    }
    case OS::ChangeType::DECREMENT: {
      for (size_t i = 0; i < change.modifiedRegisters.size(); i++) {
        mappedRegisterFileSet_.set(
            change.modifiedRegisters[i],
            mappedRegisterFileSet_.get(change.modifiedRegisters[i])
                    .get<uint64_t>() -
                change.modifiedRegisterValues[i].get<uint64_t>());
      }
      break;
    }
    default: {  // OS::ChangeType::REPLACEMENT
      // If type is ChangeType::REPLACEMENT, set new values
      for (size_t i = 0; i < change.modifiedRegisters.size(); i++) {
        mappedRegisterFileSet_.set(change.modifiedRegisters[i],
                                   change.modifiedRegisterValues[i]);
      }
      break;
    }
  }

  // Update memory
  // TODO: Analyse if ChangeType::INCREMENT or ChangeType::DECREMENT case is
  // required for memory changes
  for (size_t i = 0; i < change.memoryAddresses.size(); i++) {
    dataMemory_.requestWrite(change.memoryAddresses[i],
                             change.memoryAddressValues[i]);
  }
}

const ArchitecturalRegisterFileSet& Core::getArchitecturalRegisterFileSet()
    const {
  return mappedRegisterFileSet_;
}

void Core::sendSyscall(OS::SyscallInfo syscallInfo) const {
  handleSyscall_(syscallInfo);
}

void Core::receiveSyscallResult(const OS::SyscallResult result) const {
  exceptionHandler_->processSyscallResult(result);
}

uint64_t Core::getInstructionsRetiredCount() const {
  return reorderBuffer_.getInstructionsCommittedCount();
}

std::map<std::string, std::string> Core::getStats() const {
  auto retired = reorderBuffer_.getInstructionsCommittedCount();
  auto ipc = retired / static_cast<double>(ticks_);
  std::ostringstream ipcStr;
  ipcStr << std::fixed << std::setprecision(4) << ipc;

  auto branchStalls = fetchUnit_.getBranchStalls();

  auto earlyFlushes = decodeUnit_.getEarlyFlushes();

  auto allocationStalls = renameUnit_.getAllocationStalls();
  auto robStalls = renameUnit_.getROBStalls();
  auto lqStalls = renameUnit_.getLoadQueueStalls();
  auto sqStalls = renameUnit_.getStoreQueueStalls();

  auto rsStalls = dispatchIssueUnit_.getRSStalls();
  auto frontendStalls = dispatchIssueUnit_.getFrontendStalls();
  auto backendStalls = dispatchIssueUnit_.getBackendStalls();
  auto portBusyStalls = dispatchIssueUnit_.getPortBusyStalls();

  uint64_t totalBranchesExecuted = 0;
  uint64_t totalBranchMispredicts = 0;

  // Sum up the branch stats reported across the execution units.
  for (auto& eu : executionUnits_) {
    totalBranchesExecuted += eu.getBranchExecutedCount();
    totalBranchMispredicts += eu.getBranchMispredictedCount();
  }
  // Speculative-path mispredict rate (every wrong-path branch that reaches
  // execute, including ones later squashed). Useful as a diagnostic but not
  // comparable to silicon's BR_MIS_PRED_RETIRED counter.
  auto branchMissRateSpec =
      100.0f * static_cast<float>(totalBranchMispredicts) /
      static_cast<float>(totalBranchesExecuted);
  std::ostringstream branchMissRateSpecStr;
  branchMissRateSpecStr << std::setprecision(3) << branchMissRateSpec << "%";

  // Retired (commit-time) mispredict rate — gates on actually-committed
  // branches, matches silicon's retire-time PMU semantics. This is the
  // honest predictor-quality number. Under deep-pipeline / high-stall
  // configs (e.g. SST cache hierarchy) the spec rate inflates massively
  // from wrong-path squashes; the retired rate stays representative.
  auto retiredMispredicts = reorderBuffer_.getBranchMispredictedCount();
  auto retiredBranches = reorderBuffer_.getRetiredBranchesCount();
  float branchMissRate = retiredBranches > 0
      ? 100.0f * static_cast<float>(retiredMispredicts) /
            static_cast<float>(retiredBranches)
      : 0.0f;
  std::ostringstream branchMissRateStr;
  branchMissRateStr << std::setprecision(3) << branchMissRate << "%";

  std::map<std::string, std::string> stats =
      {{"cycles", std::to_string(ticks_)},
          {"retired", std::to_string(retired)},
          {"ipc", ipcStr.str()},
          {"flushes", std::to_string(flushes_)},
          {"fetch.branchStalls", std::to_string(branchStalls)},
          {"decode.earlyFlushes", std::to_string(earlyFlushes)},
          {"rename.allocationStalls", std::to_string(allocationStalls)},
          {"rename.robStalls", std::to_string(robStalls)},
          {"rename.lqStalls", std::to_string(lqStalls)},
          {"rename.sqStalls", std::to_string(sqStalls)},
          {"dispatch.rsStalls", std::to_string(rsStalls)},
          {"issue.frontendStalls", std::to_string(frontendStalls)},
          {"issue.backendStalls", std::to_string(backendStalls)},
          {"issue.portBusyStalls", std::to_string(portBusyStalls)},
          {"branch.executed", std::to_string(totalBranchesExecuted)},
          {"branch.mispredict", std::to_string(totalBranchMispredicts)},
          {"branch.missrate", branchMissRateStr.str()},
          {"branch.retired", std::to_string(retiredBranches)},
          {"branch.retiredMispredict", std::to_string(retiredMispredicts)},
          {"branch.specMissrate", branchMissRateSpecStr.str()},
          {"lsq.loadViolations",
           std::to_string(reorderBuffer_.getViolatingLoadsCount())},
          {"idle.ticks", std::to_string(idle_ticks_)},
          {"context.switches", std::to_string(contextSwitches_)}};

  // Predictor diagnostics (env-gated; default empty map).
  for (const auto& kv : branchPredictor_.getDiagnostics()) {
    stats[kv.first] = std::to_string(kv.second);
  }

  // Optional fetch/branch-stall diagnostics (no behavioural effect).
  if (std::getenv("SIMENG_FETCH_PROFILE") != nullptr) {
    for (const auto& kv : fetchUnit_.getFetchProfile())
      stats[kv.first] = kv.second;
  }

  // Optional LSQ load-latency / memory-level-parallelism diagnostics
  // (no behavioural effect).
  if (std::getenv("SIMENG_LSQ_PROFILE") != nullptr) {
    for (const auto& kv : loadStoreQueue_.getLsqProfile())
      stats[kv.first] = kv.second;
  }

  // Optional per-RS dispatch-stall attribution (no behavioural effect).
  if (std::getenv("SIMENG_RS_PROFILE") != nullptr) {
    const auto& full = dispatchIssueUnit_.getPerRsFullCycles();
    const auto& occ = dispatchIssueUnit_.getPerRsOccSum();
    const auto& blk = dispatchIssueUnit_.getPerRsBlockCount();
    const auto& byCap = dispatchIssueUnit_.getPerRsBlockByCapacity();
    const auto& byDR = dispatchIssueUnit_.getPerRsBlockByDispatchRate();
    auto rsN = dispatchIssueUnit_.getRsCount();
    for (uint16_t i = 0; i < rsN; i++) {
      std::string p = "rsprof.rs" + std::to_string(i) + ".";
      stats[p + "capacity"] =
          std::to_string(dispatchIssueUnit_.getRsCapacity(i));
      stats[p + "dispatchRate"] =
          std::to_string(dispatchIssueUnit_.getRsDispatchRate(i));
      stats[p + "fullCycles"] = std::to_string(full[i]);
      stats[p + "avgOccupancy"] =
          std::to_string(ticks_ > 0 ? static_cast<double>(occ[i]) / ticks_
                                    : 0.0);
      stats[p + "blockCount"] = std::to_string(blk[i]);
      stats[p + "blockByCapacity"] = std::to_string(byCap[i]);
      stats[p + "blockByDispatchRate"] = std::to_string(byDR[i]);
      stats[p + "dispatched"] = std::to_string(
          dispatchIssueUnit_.getPerRsDispatched()[i]);
      stats[p + "dispatchedCrossRS"] = std::to_string(
          dispatchIssueUnit_.getPerRsDispatchedCrossRS()[i]);
    }
    stats["rsprof.bandwidthLimitedCycles"] =
        std::to_string(dispatchIssueUnit_.getBandwidthLimitedCycles());
    stats["dispprof.slotsTried"] =
        std::to_string(dispatchIssueUnit_.getDispatchSlotsTried());
    stats["dispprof.slotsDispatched"] =
        std::to_string(dispatchIssueUnit_.getDispatchSlotsDispatched());
    stats["dispprof.earlyReturnTicks"] =
        std::to_string(dispatchIssueUnit_.getDispatchEarlyReturnTicks());
    stats["dispprof.slotsSkippedByEarlyReturn"] =
        std::to_string(dispatchIssueUnit_.getDispatchSlotsSkippedByEarlyReturn());
    {
      std::ostringstream oss;
      oss << std::hex << "0x" << reorderBuffer_.lastCommittedPC_;
      stats["pcprof.lastPC"] = oss.str();
    }
    stats["pcprof.bucket0_premain"] = std::to_string(reorderBuffer_.pcBucket_[0]);
    stats["pcprof.bucket1_main"] = std::to_string(reorderBuffer_.pcBucket_[1]);
    stats["pcprof.bucket2_libc"] = std::to_string(reorderBuffer_.pcBucket_[2]);
    stats["pcprof.bucket3_other"] = std::to_string(reorderBuffer_.pcBucket_[3]);
  }

  // Optional per-PC branch-miss bucket (Probe 1).
  if (std::getenv("SIMENG_BRANCH_PROFILE") != nullptr) {
    const auto& m = reorderBuffer_.branchMissByPc_;
    std::vector<std::pair<uint64_t, std::pair<uint64_t,uint64_t>>> v(
        m.begin(), m.end());
    // Sort by absolute miss count desc.
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b){
      return a.second.second > b.second.second;
    });
    uint64_t totalRes = 0, totalMiss = 0;
    for (const auto& kv : m) {
      totalRes += kv.second.first;
      totalMiss += kv.second.second;
    }
    stats["branchprof.totalResolutions"] = std::to_string(totalRes);
    stats["branchprof.totalMisses"] = std::to_string(totalMiss);
    stats["branchprof.distinctPcs"] = std::to_string(v.size());
    int top = std::min<int>(20, v.size());
    for (int i = 0; i < top; i++) {
      std::ostringstream pcs;
      pcs << "branchprof.pc." << std::hex << v[i].first;
      double rate = v[i].second.first
                        ? (100.0 * v[i].second.second / v[i].second.first)
                        : 0.0;
      std::ostringstream rs;
      rs << std::fixed << std::setprecision(2) << rate;
      stats[pcs.str() + ".total"] = std::to_string(v[i].second.first);
      stats[pcs.str() + ".miss"] = std::to_string(v[i].second.second);
      stats[pcs.str() + ".rate"] = rs.str();
    }
  }

  // Optional ROB head-of-line stall attribution (Probe 5).
  if (std::getenv("SIMENG_ROB_HOL_PROFILE") != nullptr) {
    uint64_t totalHol = 0;
    for (int k = 0; k < 6; k++) totalHol += reorderBuffer_.holCycleByClass_[k];
    stats["holprof.totalStallCycles"] = std::to_string(totalHol);
    for (int k = 0; k < 6; k++) {
      std::string base = std::string("holprof.") +
                         pipeline::ReorderBuffer::holClassName_[k];
      uint64_t cyc = reorderBuffer_.holCycleByClass_[k];
      stats[base + ".cycles"] = std::to_string(cyc);
      double pct = totalHol ? (100.0 * cyc / totalHol) : 0.0;
      std::ostringstream ps;
      ps << std::fixed << std::setprecision(2) << pct;
      stats[base + ".pct"] = ps.str();
      // Top 10 PCs per class.
      const auto& pm = reorderBuffer_.holPcByClass_[k];
      std::vector<std::pair<uint64_t,uint64_t>> v(pm.begin(), pm.end());
      std::sort(v.begin(), v.end(),
                [](const auto& a, const auto& b){ return a.second > b.second; });
      int top = std::min<int>(10, v.size());
      for (int i = 0; i < top; i++) {
        std::ostringstream pcs;
        pcs << base << ".pc." << std::hex << v[i].first;
        stats[pcs.str()] = std::to_string(v[i].second);
      }
    }
  }

  // Optional per-RS dispatch profiling (no behavioural effect).
  if (std::getenv("SIMENG_RS_PROFILE") != nullptr) {
    const auto& full = dispatchIssueUnit_.getPerRsFullCycles();
    const auto& occ = dispatchIssueUnit_.getPerRsOccSum();
    const auto& blk = dispatchIssueUnit_.getPerRsBlockCount();
    uint16_t n = dispatchIssueUnit_.getRsCount();
    uint64_t cyc = ticks_ ? ticks_ : 1;
    for (uint16_t i = 0; i < n; i++) {
      std::ostringstream avg;
      avg << std::fixed << std::setprecision(2)
          << (static_cast<double>(occ[i]) / static_cast<double>(cyc));
      stats["rs[" + std::to_string(i) + "].cap"] =
          std::to_string(dispatchIssueUnit_.getRsCapacity(i));
      stats["rs[" + std::to_string(i) + "].fullCycles"] =
          std::to_string(full[i]);
      stats["rs[" + std::to_string(i) + "].avgOccupancy"] = avg.str();
      stats["rs[" + std::to_string(i) + "].blockOnStall"] =
          std::to_string(blk[i]);
    }
  }

  return stats;
}

void Core::schedule(simeng::OS::cpuContext newContext) {
  // Need to reset mapping in register file
  registerAliasTable_.reset(isa_.getRegisterFileStructures(),
                            physicalRegisterQuantities_);

  currentTID_ = newContext.TID;
  fetchUnit_.setProgramLength(newContext.progByteLen);
  fetchUnit_.updatePC(newContext.pc);
  for (size_t type = 0; type < newContext.regFile.size(); type++) {
    for (size_t tag = 0; tag < newContext.regFile[type].size(); tag++) {
      mappedRegisterFileSet_.set({(uint8_t)type, (uint16_t)tag},
                                 newContext.regFile[type][tag]);
    }
  }
  status_ = CoreStatus::executing;
  procTicks_ = 0;
  isa_.updateAfterContextSwitch(newContext);
  mmu_->setTid(currentTID_);
  // Allow fetch unit to resume fetching instructions & incrementing PC
  fetchUnit_.unpause();
}

bool Core::interrupt() {
  if (exceptionGenerated_ == false) {
    status_ = CoreStatus::switching;
    contextSwitches_++;
    // Stop fetch unit from incrementing PC or fetching next instructions
    fetchUnit_.pause();
    return true;
  }
  return false;
}

uint64_t Core::getCurrentProcTicks() const { return procTicks_; }

simeng::OS::cpuContext Core::getCurrentContext() const {
  OS::cpuContext newContext;
  newContext.TID = currentTID_;
  newContext.pc =
      exceptionGenerated_
          ? exceptionGeneratingInstruction_->getInstructionAddress() + 4
          : fetchUnit_.getPC();
  // progByteLen will not change in process so do not need to set it
  // Don't need to explicitly save SP as will be in reg file contents
  auto regFileStruc = isa_.getRegisterFileStructures();
  newContext.regFile.resize(regFileStruc.size());
  for (size_t i = 0; i < regFileStruc.size(); i++) {
    newContext.regFile[i].resize(regFileStruc[i].quantity);
  }
  // Set all reg Values
  for (size_t type = 0; type < newContext.regFile.size(); type++) {
    for (size_t tag = 0; tag < newContext.regFile[type].size(); tag++) {
      newContext.regFile[type][tag] =
          mappedRegisterFileSet_.get({(uint8_t)type, (uint16_t)tag});
    }
  }
  // Do not need to explicitly set newContext.sp as it will be included in
  // regFile
  return newContext;
}

}  // namespace outoforder
}  // namespace models
}  // namespace simeng
