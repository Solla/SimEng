#include "simeng/pipeline/LoadStoreQueue.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>

namespace simeng {
namespace pipeline {

/** Check whether requests `a` and `b` overlap. */
bool requestsOverlap(memory::MemoryAccessTarget a,
                     memory::MemoryAccessTarget b) {
  // Check whether one region ends before the other begins, implying no overlap,
  // and negate
  return !(a.address + a.size <= b.address || b.address + b.size <= a.address);
}

LoadStoreQueue::LoadStoreQueue(
    unsigned int maxCombinedSpace, memory::MemoryInterface& memory,
    span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
    std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
    std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
    bool exclusive, uint16_t loadBandwidth, uint16_t storeBandwidth,
    uint16_t permittedRequests, uint16_t permittedLoads,
    uint16_t permittedStores, bool l2lForwarding, uint64_t l2lForwardLatency)
    : completionSlots_(completionSlots),
      forwardOperands_(forwardOperands),
      raiseException_(raiseException),
      maxCombinedSpace_(maxCombinedSpace),
      combined_(true),
      memory_(memory),
      exclusive_(exclusive),
      loadBandwidth_(loadBandwidth),
      storeBandwidth_(storeBandwidth),
      totalLimit_(permittedRequests),
      // Set per-cycle limits for each request type
      reqLimits_{permittedLoads, permittedStores} {
  lsqProfile_ = std::getenv("SIMENG_LSQ_PROFILE") != nullptr;
  l2lForward_ = l2lForwarding;
  l2lLatency_ = l2lForwardLatency;
}

LoadStoreQueue::LoadStoreQueue(
    unsigned int maxLoadQueueSpace, unsigned int maxStoreQueueSpace,
    memory::MemoryInterface& memory,
    span<PipelineBuffer<std::shared_ptr<Instruction>>> completionSlots,
    std::function<void(span<Register>, span<RegisterValue>)> forwardOperands,
    std::function<void(const std::shared_ptr<Instruction>&)> raiseException,
    bool exclusive, uint16_t loadBandwidth, uint16_t storeBandwidth,
    uint16_t permittedRequests, uint16_t permittedLoads,
    uint16_t permittedStores, bool l2lForwarding, uint64_t l2lForwardLatency)
    : completionSlots_(completionSlots),
      forwardOperands_(forwardOperands),
      raiseException_(raiseException),
      maxLoadQueueSpace_(maxLoadQueueSpace),
      maxStoreQueueSpace_(maxStoreQueueSpace),
      combined_(false),
      memory_(memory),
      exclusive_(exclusive),
      loadBandwidth_(loadBandwidth),
      storeBandwidth_(storeBandwidth),
      totalLimit_(permittedRequests),
      // Set per-cycle limits for each request type
      reqLimits_{permittedLoads, permittedStores} {
  lsqProfile_ = std::getenv("SIMENG_LSQ_PROFILE") != nullptr;
  l2lForward_ = l2lForwarding;
  l2lLatency_ = l2lForwardLatency;
}

unsigned int LoadStoreQueue::getLoadQueueSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getLoadQueueSplitSpace();
  }
}
unsigned int LoadStoreQueue::getStoreQueueSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getStoreQueueSplitSpace();
  }
}
unsigned int LoadStoreQueue::getTotalSpace() const {
  if (combined_) {
    return getCombinedSpace();
  } else {
    return getLoadQueueSplitSpace() + getStoreQueueSplitSpace();
  }
}

unsigned int LoadStoreQueue::getLoadQueueSplitSpace() const {
  return maxLoadQueueSpace_ - loadQueue_.size();
}
unsigned int LoadStoreQueue::getStoreQueueSplitSpace() const {
  return maxStoreQueueSpace_ - storeQueue_.size();
}
unsigned int LoadStoreQueue::getCombinedSpace() const {
  return maxCombinedSpace_ - loadQueue_.size() - storeQueue_.size();
}

void LoadStoreQueue::addLoad(const std::shared_ptr<Instruction>& insn) {
  loadQueue_.push_back(insn);
}
void LoadStoreQueue::addStore(const std::shared_ptr<Instruction>& insn) {
  storeQueue_.push_back({insn, {}});
}

bool LoadStoreQueue::loadHeldOnAddr(
    uint64_t loadSeqId, const memory::MemoryAccessTarget& loadReq) const {
  // startLoad() registers a held (forwarded) load under the exact address it
  // forwards from (itLd->address == store address). The violation scan
  // iterates the same generated load addresses, so an exact-address match is
  // sufficient.
  for (const auto& storeEntry : conflictionMap_) {
    const auto addrEntry = storeEntry.second.find(loadReq.address);
    if (addrEntry == storeEntry.second.end()) continue;
    for (const auto& p : addrEntry->second) {
      if (p.first->getSequenceId() == loadSeqId) return true;
    }
  }
  return false;
}

bool LoadStoreQueue::olderStoreHazard(
    const std::shared_ptr<Instruction>& load) const {
  const uint64_t loadSeqId = load->getSequenceId();
  const auto& ldAddrs = load->getGeneratedAddresses();
  for (const auto& entry : storeQueue_) {
    const auto& store = entry.first;
    if (store->getSequenceId() >= loadSeqId) continue;  // not older
    const auto sAddrs = store->getGeneratedAddresses();
    // Older store with an unresolved address: cannot disambiguate at all.
    if (sAddrs.size() == 0) return true;
    // Older store with a resolved address: a hazard only if it overlaps the
    // load and the overlap is not exactly store-to-load forwardable (the
    // forwarding path in issueLoad() requires identical base address and
    // load.size <= store.size). Anything else (wider load, partial overlap)
    // would issue a speculative memory read of stale data.
    for (const auto& s : sAddrs) {
      for (const auto& l : ldAddrs) {
        if (requestsOverlap(s, l) &&
            !(l.address == s.address && l.size <= s.size)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool LoadStoreQueue::anyOlderStoreOverlaps(
    const std::shared_ptr<Instruction>& load) const {
  const uint64_t loadSeqId = load->getSequenceId();
  const auto& ldAddrs = load->getGeneratedAddresses();
  for (const auto& entry : storeQueue_) {
    const auto& store = entry.first;
    if (store->getSequenceId() >= loadSeqId) continue;  // not older
    const auto sAddrs = store->getGeneratedAddresses();
    // Unresolved older store: cannot rule out an overlap.
    if (sAddrs.size() == 0) return true;
    for (const auto& s : sAddrs)
      for (const auto& l : ldAddrs)
        if (requestsOverlap(s, l)) return true;
  }
  return false;
}

void LoadStoreQueue::startLoad(const std::shared_ptr<Instruction>& insn) {
  // Memory-dependence prediction: a load whose instruction address has
  // previously caused a memory-order violation is conservatively held while
  // it still hazards an older in-flight store, instead of issuing
  // speculatively and triggering a violation + pipeline flush every loop
  // iteration (an unbounded storm on RAW-through-memory hazards). A load's
  // first execution (PC not yet a known violator) issues normally, so
  // baseline violation detection/behaviour is preserved.
  // Cheap predictor lookup first: a load PC that has never violated takes the
  // exact original path (issueLoad only), preserving baseline behaviour and
  // call patterns. Only a known prior violator pays the hazard check.
  if (memDepViolatorPCs_.count(insn->getInstructionAddress()) &&
      insn->getGeneratedAddresses().size() != 0 && olderStoreHazard(insn)) {
    pendingDisambiguation_.push_back(insn);
    return;
  }
  issueLoad(insn);
}

void LoadStoreQueue::issueLoad(const std::shared_ptr<Instruction>& insn) {
  const auto& ld_addresses = insn->getGeneratedAddresses();
  if (ld_addresses.size() == 0) {
    // Early execution if not addresses need to be accessed
    insn->execute();

    // A load exception (e.g. data abort) must NOT be raised eagerly here: this
    // load may be speculative / on a mispredicted path. Route it through
    // completedLoads_ so it reaches the ReorderBuffer in program order, where
    // the exception is raised at commit (non-speculative) or discarded if the
    // load is flushed first.
    if (lsqProfile_) lpLoadsEarly_++;
    completedLoads_.push(insn);
  } else {
    // Load-to-load forwarding HEADROOM probe (profiling only): would this load's
    // address have matched an older in-flight load, or a recently-accessed
    // word/line? High hit rates => a forwarding implementation has headroom.
    if (lsqProfile_ && ld_addresses.size() != 0) {
      const uint64_t word = ld_addresses[0].address & ~uint64_t(7);
      const uint64_t line = ld_addresses[0].address & ~uint64_t(63);
      bool inflExact = false, inflLine = false;
      for (const auto& kv : requestedLoads_) {
        if (kv.second->getSequenceId() >= insn->getSequenceId()) continue;
        for (const auto& a : kv.second->getGeneratedAddresses()) {
          if ((a.address & ~uint64_t(7)) == word) inflExact = true;
          if ((a.address & ~uint64_t(63)) == line) inflLine = true;
        }
        if (inflExact) break;
      }
      lpL2LInflightExact_ += inflExact;
      lpL2LInflightLine_ += inflLine;
      bool recExact = false, recLine = false;
      for (uint64_t w : lpRecentWords_)
        if (w == word) { recExact = true; break; }
      for (uint64_t l : lpRecentLines_)
        if (l == line) { recLine = true; break; }
      lpL2LRecentExact_ += recExact;
      lpL2LRecentLine_ += recLine;
      lpRecentWords_.push_back(word);
      lpRecentLines_.push_back(line);
      if (lpRecentWords_.size() > 1024) lpRecentWords_.pop_front();
      if (lpRecentLines_.size() > 1024) lpRecentLines_.pop_front();
    }
    // --- Load-to-load forwarding: serve a single-access load from the
    // forwarding cache when its exact address+size was recently produced by a
    // returning load and NO older store overlaps it. The no-overlap check must
    // be stricter than olderStoreHazard() (which permits exactly-forwardable
    // overlaps because the normal path store-to-load-forwards them): if we
    // forwarded those from the cache we would supply stale memory data instead
    // of the pending store's value. Completes after l2lLatency_ cycles without
    // a memory access. Multi-access loads fall through to the memory path. ---
    if (l2lForward_ && ld_addresses.size() == 1) {
      const auto& target = ld_addresses[0];
      auto fit = forwardingCache_.find(target.address);
      // Only run the (more expensive) older-store overlap scan once the cache is
      // known to hold a wide-enough entry for this load. On the common miss path
      // this skips the scan entirely, leaving the load's call pattern identical
      // to a plain memory issue; the && is short-circuit so the forwarding
      // decision is unchanged.
      if (fit != forwardingCache_.end() && fit->second.first >= target.size &&
          !anyOlderStoreOverlaps(insn)) {
        // Supply exactly the load's access size (the memory path always
        // returns request-sized data); a larger cached value would corrupt the
        // load result.
        insn->supplyData(target.address,
                         fit->second.second.zeroExtend(target.size, target.size));
        if (insn->hasAllData()) {
          insn->execute();
          if (!insn->exceptionEncountered() && insn->isStoreData())
            supplyStoreData(insn);
          forwardCompletionQueue_[tickCounter_ + l2lLatency_].push_back(insn);
          l2lForwarded_++;
          if (lsqProfile_) lpLoadsIssued_++;
          return;
        }
      }
    }
    // Issue the memory request as soon as the address is generated; the
    // memory interface (FixedLatency standalone, or the SST cache element
    // when running under SST) owns ALL access latency. Previously this
    // scheduled the request at tickCounter_ + getLSQLatency() (the load
    // uop's pipeline Execution-Latency), which was then summed in series
    // with the memory interface's own Access-Latency for the SAME physical
    // L1 access — double-counting one latency (8cy floor for a ~4cy L1
    // load-to-use; see lsqprof). Issuing immediately keeps the model
    // single-sourced and SST-compatible (SST would otherwise be
    // double-counted too).
    const uint64_t readyTick = tickCounter_;
    // Create a speculative entry for the load
    requestLoadQueue_[readyTick].push_back({{}, insn});
    // Store a reference to the reqAddresses queue for easy access
    auto& reqAddrQueue = requestLoadQueue_[readyTick].back().reqAddresses;
    // Store load addresses temporarily so that conflictions are
    // only registered once on most recent (program order) store
    std::list<simeng::memory::MemoryAccessTarget> temp_load_addr(
        ld_addresses.begin(), ld_addresses.end());

    // Detect reordering conflicts
    if (storeQueue_.size() > 0) {
      uint64_t seqId = insn->getSequenceId();
      for (auto itSt = storeQueue_.rbegin(); itSt != storeQueue_.rend();
           itSt++) {
        const auto& store = itSt->first;
        // If entry is earlier in the program order than load, detect conflicts
        if (store->getSequenceId() < seqId) {
          const auto& str_addresses = store->getGeneratedAddresses();
          // Iterate over possible matches between store and load addresses
          for (const auto& str : str_addresses) {
            auto itLd = temp_load_addr.begin();
            while (itLd != temp_load_addr.end()) {
              // If conflict exists, register in conflictionMap_ and delay
              // load request(s) until conflicting store retires
              if (itLd->address == str.address) {
                // Load access size must be no larger than the store access size
                // to ensure all data is encapsulated in the later forwarding
                if (itLd->size <= str.size) {
                  conflictionMap_[store->getSequenceId()][str.address]
                      .push_back({insn, itLd->size});
                } else {
                  // To ensure load doesn't match on an earlier store, generate
                  // load request for address
                  reqAddrQueue.push(*itLd);
                }
                // Remove from temporary vector so the confliction can't be
                // registered again
                itLd = temp_load_addr.erase(itLd);
              } else {
                itLd++;
              }
            }
          }
        }
      }
    }
    // If addresses remain that had no conflictions, generate those load
    // request(s)
    for (const auto& ld_addr : temp_load_addr) reqAddrQueue.emplace(ld_addr);

    // Register active load
    requestedLoads_.emplace(insn->getSequenceId(), insn);

    if (lsqProfile_) {
      lpLoadsIssued_++;
      lpInFlight_++;
      lpIssueTick_[insn->getSequenceId()] = tickCounter_;
    }
  }
}

void LoadStoreQueue::supplyStoreData(const std::shared_ptr<Instruction>& insn) {
  if (!insn->isStoreData()) return;
  // Get identifier values
  const uint64_t macroOpNum = insn->getInstructionId();
  const int microOpNum = insn->getMicroOpIndex();

  // Get data
  span<const simeng::RegisterValue> data = insn->getData();

  // Find storeQueue_ entry which is linked to the store data operation
  auto itSt = storeQueue_.begin();
  while (itSt != storeQueue_.end()) {
    auto& entry = itSt->first;
    // Pair entry and incoming store data operation with macroOp identifier and
    // microOp index value pre-determined in microDecoder
    if (entry->getInstructionId() == macroOpNum &&
        entry->getMicroOpIndex() == microOpNum) {
      // Supply data to be stored by operations
      itSt->second = data;
      break;
    } else {
      itSt++;
    }
  }
}

bool LoadStoreQueue::commitStore(const std::shared_ptr<Instruction>& uop) {
  assert(storeQueue_.size() > 0 &&
         "Attempted to commit a store from an empty queue");
  assert(storeQueue_.front().first->getSequenceId() == uop->getSequenceId() &&
         "Attempted to commit a store that wasn't present at the front of the "
         "store queue");

  const auto& addresses = uop->getGeneratedAddresses();
  span<const simeng::RegisterValue> data = storeQueue_.front().second;

  // Early exit if there's no addresses to process
  if (addresses.size() == 0) {
    storeQueue_.pop_front();
    return false;
  }

  // Schedule the store request immediately; the memory interface owns access
  // latency (see issueLoad — single-sourced, SST-compatible).
  const uint64_t readyTick = tickCounter_;
  requestStoreQueue_[readyTick].push_back({{}, uop});
  // Submit request write to memory interface early as the architectural state
  // considers the store to be retired and thus its operation complete
  for (size_t i = 0; i < addresses.size(); i++) {
    memory_.requestWrite(addresses[i], data[i]);
    // Still add addresses to requestQueue_ to ensure contention of resources is
    // correctly simulated
    requestStoreQueue_[readyTick].back().reqAddresses.push(addresses[i]);
    // Invalidate any forwarding-cache line(s) this store overwrites so a later
    // load to the same address cannot forward stale data.
    if (l2lForward_ && !forwardingCache_.empty()) {
      for (auto cit = forwardingCache_.begin(); cit != forwardingCache_.end();) {
        const memory::MemoryAccessTarget cached{cit->first, cit->second.first};
        if (requestsOverlap(addresses[i], cached))
          cit = forwardingCache_.erase(cit);
        else
          ++cit;
      }
    }
  }

  // Check all loads that have requested memory
  violatingLoad_ = nullptr;
  for (const auto& load : requestedLoads_) {
    // Skip loads that are younger than the oldest violating load
    if (violatingLoad_ &&
        load.second->getSequenceId() > violatingLoad_->getSequenceId())
      continue;
    // Violation invalid if the load and store entries are generated by the same
    // uop
    // Only a load younger (later in program order) than the committing store
    // can have read stale data from it.
    if (load.second->getSequenceId() > uop->getSequenceId()) {
      const auto& loadedAddresses = load.second->getGeneratedAddresses();
      // Iterate over store addresses
      for (const auto& storeReq : addresses) {
        // Iterate over load addresses
        for (const auto& loadReq : loadedAddresses) {
          // Check for overlapping requests, and flush if discovered. A load
          // held in conflictionMap_ for this address is awaiting
          // store-to-load forwarding and never issued a memory read there, so
          // it is not a violation (excluding it is what prevents an unbounded
          // forwarding/violation storm on RAW-through-memory hazards).
          if (requestsOverlap(storeReq, loadReq) &&
              !loadHeldOnAddr(load.second->getSequenceId(), loadReq)) {
            violatingLoad_ = load.second;
            // Record this load PC so the memory-dependence predictor holds it
            // on future executions, preventing a per-iteration storm.
            memDepViolatorPCs_.insert(load.second->getInstructionAddress());
          }
        }
      }
    }
  }

  // Resolve any conflicts caused by this store instruction
  const auto& itSt = conflictionMap_.find(uop->getSequenceId());
  if (itSt != conflictionMap_.end()) {
    for (size_t i = 0; i < addresses.size(); i++) {
      const auto& itAddr = itSt->second.find(addresses[i].address);
      if (itAddr != itSt->second.end()) {
        for (const auto& pair : itAddr->second) {
          const auto& load = pair.first;
          load->supplyData(addresses[i].address,
                           data[i].zeroExtend(
                               std::min(pair.second, (uint16_t)data[i].size()),
                               pair.second));
          if (load->hasAllData()) {
            // This load has completed
            load->execute();
            if (load->isStoreData()) {
              supplyStoreData(load);
            }
            lpRecordComplete_(load, /*viaForward=*/true);
            completedLoads_.push(load);
          }
        }
      }
    }
    conflictionMap_.erase(itSt);
  }

  storeQueue_.pop_front();

  return violatingLoad_ != nullptr;
}

void LoadStoreQueue::commitLoad(const std::shared_ptr<Instruction>& uop) {
  assert(loadQueue_.size() > 0 &&
         "Attempted to commit a load from an empty queue");
  assert(loadQueue_.front()->getSequenceId() == uop->getSequenceId() &&
         "Attempted to commit a load that wasn't present at the front of the "
         "load queue");

  auto it = loadQueue_.begin();
  while (it != loadQueue_.end()) {
    const auto& entry = *it;
    if (entry->isLoad()) {
      requestedLoads_.erase(entry->getSequenceId());
      it = loadQueue_.erase(it);
      break;
    } else {
      it++;
    }
  }
}

void LoadStoreQueue::purgeFlushed() {
  // Drop flushed load-to-load forwarded loads awaiting completion. (The tick
  // loop also skips flushed entries, but pruning here bounds the structure.)
  if (l2lForward_) {
    auto itF = forwardCompletionQueue_.begin();
    while (itF != forwardCompletionQueue_.end()) {
      auto& vec = itF->second;
      vec.erase(std::remove_if(vec.begin(), vec.end(),
                               [](const std::shared_ptr<Instruction>& i) {
                                 return i->isFlushed();
                               }),
                vec.end());
      if (vec.empty())
        itF = forwardCompletionQueue_.erase(itF);
      else
        ++itF;
    }
  }

  // Drop flushed loads held by the memory-dependence predictor
  {
    auto it = pendingDisambiguation_.begin();
    while (it != pendingDisambiguation_.end()) {
      if ((*it)->isFlushed())
        it = pendingDisambiguation_.erase(it);
      else
        it++;
    }
  }

  // Remove flushed loads from load queue
  auto itLd = loadQueue_.begin();
  while (itLd != loadQueue_.end()) {
    const auto& entry = *itLd;
    if (entry->isFlushed()) {
      if (lsqProfile_) {
        auto pit = lpIssueTick_.find(entry->getSequenceId());
        if (pit != lpIssueTick_.end()) {
          lpIssueTick_.erase(pit);
          if (lpInFlight_ > 0) lpInFlight_--;
        }
      }
      requestedLoads_.erase(entry->getSequenceId());
      itLd = loadQueue_.erase(itLd);
    } else {
      itLd++;
    }
  }

  // Remove flushed stores from store queue and confliction queue if an entry
  // exists
  auto itSt = storeQueue_.begin();
  while (itSt != storeQueue_.end()) {
    const auto& entry = itSt->first;
    if (entry->isFlushed()) {
      conflictionMap_.erase(entry->getSequenceId());
      itSt = storeQueue_.erase(itSt);
    } else {
      itSt++;
    }
  }

  // Remove flushed loads from confliction queue
  for (auto itCnflct = conflictionMap_.begin();
       itCnflct != conflictionMap_.end(); itCnflct++) {
    // Iterate over addresses of store
    for (auto itAddr = itCnflct->second.begin();
         itAddr != itCnflct->second.end(); itAddr++) {
      // Iterate over vector of instructions conflicting with store address
      auto pair = itAddr->second.begin();
      while (pair != itAddr->second.end()) {
        if (pair->first->isFlushed()) {
          pair = itAddr->second.erase(pair);
        } else {
          pair++;
        }
      }
    }
  }

  // Remove flushed loads and stores from request queues
  auto itLdReq = requestLoadQueue_.begin();
  while (itLdReq != requestLoadQueue_.end()) {
    auto itInsn = itLdReq->second.begin();
    while (itInsn != itLdReq->second.end()) {
      if (itInsn->insn->isFlushed()) {
        itInsn = itLdReq->second.erase(itInsn);
      } else {
        itInsn++;
      }
    }
    if (itLdReq->second.size() == 0) {
      itLdReq = requestLoadQueue_.erase(itLdReq);
    } else {
      itLdReq++;
    }
  }
  auto itStReq = requestStoreQueue_.begin();
  while (itStReq != requestStoreQueue_.end()) {
    auto itInsn = itStReq->second.begin();
    while (itInsn != itStReq->second.end()) {
      if (itInsn->insn->isFlushed()) {
        itInsn = itStReq->second.erase(itInsn);
      } else {
        itInsn++;
      }
    }
    if (itStReq->second.size() == 0) {
      itStReq = requestStoreQueue_.erase(itStReq);
    } else {
      itStReq++;
    }
  }
}

void LoadStoreQueue::tick() {
  tickCounter_++;

  // Release memory-dependence-predicted loads once they no longer hazard an
  // older in-flight store (its address resolved/forwardable, or it committed).
  // Self-draining: older stores always commit in program order, so a held
  // load cannot wait forever.
  if (!pendingDisambiguation_.empty()) {
    std::vector<std::shared_ptr<Instruction>> stillPending;
    stillPending.reserve(pendingDisambiguation_.size());
    for (auto& insn : pendingDisambiguation_) {
      if (insn->isFlushed()) continue;
      if (olderStoreHazard(insn))
        stillPending.push_back(insn);
      else
        issueLoad(insn);
    }
    pendingDisambiguation_.swap(stillPending);
  }

  // Send memory requests adhering to set bandwidth and number of permitted
  // requests per cycle
  // Index 0: loads, index 1: stores
  std::array<uint16_t, 2> reqCounts = {0, 0};
  std::array<uint64_t, 2> dataTransferred = {0, 0};
  std::array<bool, 2> exceededLimits = {false, false};
  auto itLoad = requestLoadQueue_.begin();
  auto itStore = requestStoreQueue_.begin();
  while (requestLoadQueue_.size() + requestStoreQueue_.size() > 0) {
    // Choose which request type to schedule next
    bool chooseLoad = false;
    std::pair<bool, uint64_t> earliestLoad;
    std::pair<bool, uint64_t> earliestStore;
    // Determine if a load request can be scheduled
    if (requestLoadQueue_.size() == 0 || exceededLimits[accessType::LOAD]) {
      earliestLoad = {false, 0};
    } else {
      earliestLoad = {true, itLoad->first};
    }
    // Determine if a store request can be scheduled
    if (requestStoreQueue_.size() == 0 || exceededLimits[accessType::STORE]) {
      earliestStore = {false, 0};
    } else {
      earliestStore = {true, itStore->first};
    }
    // Choose between available requests favouring those constructed earlier
    // (store requests on a tie)
    if (earliestLoad.first) {
      chooseLoad = !(earliestStore.first &&
                     (earliestLoad.second >= earliestStore.second));
    } else if (!earliestStore.first) {
      break;
    }

    // Get next request to schedule
    auto& itReq = chooseLoad ? itLoad : itStore;
    auto itInsn = itReq->second.begin();
    auto bandwidth = chooseLoad ? loadBandwidth_ : storeBandwidth_;

    // Check if earliest request is ready
    if (itReq->first <= tickCounter_) {
      // Identify request type
      uint8_t isStore = 0;
      if (!chooseLoad) {
        isStore = 1;
      }
      // If LSQ only allows one type of request within a cycle, prevent other
      // type from being scheduled
      if (exclusive_) exceededLimits[!isStore] = true;

      // Iterate over requests ready this cycle
      while (itInsn != itReq->second.end()) {
        // Schedule requests from the queue of addresses in
        // request[Load|Store]Queue_ entry
        auto& addressQueue = itInsn->reqAddresses;
        while (addressQueue.size()) {
          const simeng::memory::MemoryAccessTarget req =
              addressQueue.front();  // Speculatively increment count of this
                                     // request type
          reqCounts[isStore]++;

          // Ensure the limit on the number of permitted operations is adhered
          // to
          if (reqCounts[isStore] + reqCounts[!isStore] > totalLimit_) {
            // No more requests can be scheduled this cycle
            exceededLimits = {true, true};
            itInsn = itReq->second.end();
            break;
          } else if (reqCounts[isStore] > reqLimits_[isStore]) {
            // No more requests of this type can be scheduled this cycle
            exceededLimits[isStore] = true;
            // Remove speculative increment to ensure it doesn't count for
            // comparisons against the totalLimit_
            reqCounts[isStore]--;
            itInsn = itReq->second.end();
            break;
          }

          // Ensure the limit on the data transferred per cycle is adhered to
          assert(req.size <= bandwidth &&
                 "Individual memory request from LoadStoreQueue exceeds L1 "
                 "bandwidth set and thus will never be submitted");
          dataTransferred[isStore] += req.size;
          if (dataTransferred[isStore] > bandwidth) {
            // No more requests can be scheduled this cycle
            exceededLimits[isStore] = true;
            itInsn = itReq->second.end();
            break;
          }

          // Request a read from the memory interface if the requestQueue_
          // entry represents a read
          if (!isStore) {
            memory_.requestRead(req, itInsn->insn->getSequenceId());
          }

          // Remove processed address from queue
          addressQueue.pop();
        }
        // Remove entry from vector if all of its requests have been
        // scheduled
        if (addressQueue.size() == 0) {
          itInsn = itReq->second.erase(itInsn);
        }
      }

      // If all uops for currently selected cycle in request[Load|Store]Queue_
      // have been scheduled, erase entry
      if (itReq->second.size() == 0) {
        if (chooseLoad) {
          itReq = requestLoadQueue_.erase(itReq);
        } else {
          itReq = requestStoreQueue_.erase(itReq);
        }
      }
    } else {
      break;
    }
  }

  // Process completed read requests
  for (const auto& response : memory_.getCompletedReads()) {
    const auto& address = response.target.address;
    const auto& data = response.data;

    // TODO: Detect and handle non-fatal faults (e.g. page fault)

    // Find instruction that requested the memory read
    const auto& itr = requestedLoads_.find(response.requestId);
    if (itr == requestedLoads_.end()) {
      continue;
    }

    // Cache the returned value for load-to-load forwarding (exact address).
    if (l2lForward_) {
      auto cit = forwardingCache_.find(address);
      if (cit == forwardingCache_.end()) forwardingCacheOrder_.push_back(address);
      forwardingCache_[address] = {response.target.size, data};
      // Cap the cache; reuse is to recent addresses so an oldest-out policy is
      // sufficient and keeps lookup/memory cheap.
      while (forwardingCacheOrder_.size() > 4096) {
        forwardingCache_.erase(forwardingCacheOrder_.front());
        forwardingCacheOrder_.pop_front();
      }
    }

    // Supply data to the instruction and execute if it is ready
    const auto& load = itr->second;
    load->supplyData(address, data);
    if (load->hasAllData()) {
      // This load has completed
      load->execute();

      // A load exception (e.g. data abort) must NOT be raised eagerly here:
      // this load may be speculative / on a mispredicted path. Route it
      // through completedLoads_ so it reaches the ReorderBuffer in program
      // order, where the exception is raised at commit (non-speculative) or
      // discarded if the load is flushed first.
      if (!load->exceptionEncountered() && load->isStoreData()) {
        supplyStoreData(load);
      }
      lpRecordComplete_(load, /*viaForward=*/false);
      completedLoads_.push(load);
    }
  }
  memory_.clearCompletedReads();

  // Complete load-to-load forwarded loads whose forward latency has elapsed.
  if (l2lForward_) {
    auto itF = forwardCompletionQueue_.begin();
    while (itF != forwardCompletionQueue_.end() && itF->first <= tickCounter_) {
      for (const auto& load : itF->second) {
        if (load->isFlushed()) continue;
        lpRecordComplete_(load, /*viaForward=*/true);
        completedLoads_.push(load);
      }
      itF = forwardCompletionQueue_.erase(itF);
    }
  }

  // Pop from the front of the completed loads queue and send to writeback
  size_t count = 0;
  while (completedLoads_.size() > 0 && count < completionSlots_.size()) {
    const auto& insn = completedLoads_.front();

    // Don't process load instruction if it has been flushed
    if (insn->isFlushed()) {
      completedLoads_.pop();
      continue;
    }

    // Forward the results
    forwardOperands_(insn->getDestinationRegisters(), insn->getResults());

    completionSlots_[count].getTailSlots()[0] = std::move(insn);

    completedLoads_.pop();

    count++;
  }

  // Sample memory-level parallelism: average in-flight loads over ticks that
  // had any load outstanding. ~1.0 means dependent loads serialise (the run
  // is bound by per-load latency, not throughput); much larger means loads
  // overlap and latency is hidden.
  if (lsqProfile_ && lpInFlight_ > 0) {
    lpOutstandingSum_ += static_cast<uint64_t>(lpInFlight_);
    lpOutstandingSamples_++;
    if (static_cast<uint64_t>(lpInFlight_) > lpOutstandingMax_)
      lpOutstandingMax_ = static_cast<uint64_t>(lpInFlight_);
  }
}

std::shared_ptr<Instruction> LoadStoreQueue::getViolatingLoad() const {
  return violatingLoad_;
}

bool LoadStoreQueue::isCombined() const { return combined_; }

void LoadStoreQueue::lpRecordComplete_(
    const std::shared_ptr<Instruction>& load, bool viaForward) {
  if (!lsqProfile_) return;
  const uint64_t seqId = load->getSequenceId();
  auto it = lpIssueTick_.find(seqId);
  // A forwarded load may complete without ever reaching the memory path; it
  // still has an issue stamp from issueLoad(). If absent (e.g. completed twice
  // across multi-address supply) skip to avoid double-counting.
  if (it == lpIssueTick_.end()) return;
  const uint64_t lat = tickCounter_ - it->second;
  lpIssueTick_.erase(it);
  if (lpInFlight_ > 0) lpInFlight_--;
  lpLatSum_ += lat;
  if (viaForward)
    lpLoadsViaFwd_++;
  else
    lpLoadsViaMem_++;
  size_t b;
  if (lat == 0)
    b = 0;
  else if (lat <= 2)
    b = 1;
  else if (lat <= 4)
    b = 2;
  else if (lat <= 8)
    b = 3;
  else if (lat <= 16)
    b = 4;
  else if (lat <= 32)
    b = 5;
  else if (lat <= 64)
    b = 6;
  else
    b = 7;
  lpLatBucket_[b]++;
}

std::vector<std::pair<std::string, std::string>>
LoadStoreQueue::getLsqProfile() const {
  static const char* kBucket[8] = {"0",     "1-2",   "3-4",   "5-8",
                                   "9-16",  "17-32", "33-64", "65+"};
  std::vector<std::pair<std::string, std::string>> p;
  p.emplace_back("lsqprof.loadsIssued", std::to_string(lpLoadsIssued_));
  p.emplace_back("lsqprof.loadsEarlyNoAddr", std::to_string(lpLoadsEarly_));
  p.emplace_back("lsqprof.completedViaMem", std::to_string(lpLoadsViaMem_));
  p.emplace_back("lsqprof.completedViaForward",
                 std::to_string(lpLoadsViaFwd_));
  const uint64_t done = lpLoadsViaMem_ + lpLoadsViaFwd_;
  // Average issue->complete latency (x1000, integer-formatted).
  uint64_t avgLatMilli = done ? (lpLatSum_ * 1000ULL) / done : 0;
  p.emplace_back("lsqprof.avgLoadLatency_milli",
                 std::to_string(avgLatMilli));
  // Average in-flight loads over ticks with work = memory-level parallelism.
  // ~1.0 => dependent loads serialise (latency-bound); >>1 => they overlap.
  uint64_t avgMlpMilli =
      lpOutstandingSamples_
          ? (lpOutstandingSum_ * 1000ULL) / lpOutstandingSamples_
          : 0;
  p.emplace_back("lsqprof.avgInFlight_milli", std::to_string(avgMlpMilli));
  p.emplace_back("lsqprof.maxInFlight", std::to_string(lpOutstandingMax_));
  p.emplace_back("lsqprof.busyTicks",
                 std::to_string(lpOutstandingSamples_));
  for (size_t i = 0; i < 8; i++)
    p.emplace_back(std::string("lsqprof.lat.") + kBucket[i],
                   std::to_string(lpLatBucket_[i]));
  // Load-to-load forwarding headroom (candidates among loadsIssued).
  p.emplace_back("lsqprof.l2l.inflightExact",
                 std::to_string(lpL2LInflightExact_));
  p.emplace_back("lsqprof.l2l.inflightLine",
                 std::to_string(lpL2LInflightLine_));
  p.emplace_back("lsqprof.l2l.recentExact",
                 std::to_string(lpL2LRecentExact_));
  p.emplace_back("lsqprof.l2l.recentLine", std::to_string(lpL2LRecentLine_));
  p.emplace_back("lsqprof.l2l.forwarded", std::to_string(l2lForwarded_));
  return p;
}

}  // namespace pipeline
}  // namespace simeng
