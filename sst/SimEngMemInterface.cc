// clang-format off
// DO NOT MOVE FROM TOP OF FILE - https://github.com/sstsimulator/sst-core/issues/865
#include <sst/core/sst_config.h>
// clang-format on

#include "SimEngMemInterface.hh"

#include <iostream>

using namespace SST::SSTSimEng;

SimEngMemInterface::SimEngMemInterface(StandardMem* mem,
                                       std::shared_ptr<simeng::memory::MMU> mmu,
                                       uint64_t cl, uint64_t max_addr,
                                       bool debug)
    : simeng::memory::MemoryInterface() {
  this->sstMem_ = mem;
  this->mmu_ = mmu;
  this->cacheLineWidth_ = cl;
  this->maxAddrMemory_ = max_addr;
  this->debug_ = debug;
};

template <typename T,
          typename std::enable_if<std::is_base_of<
              SimEngMemInterface::SimEngMemoryRequest, T>::value>::type*>
std::vector<StandardMem::Request*> SimEngMemInterface::makeSSTRequests(
    T* aggrReq, uint64_t addrStart, uint64_t addrEnd, uint64_t size) {
  /*
      Here we check if the memory request spans multiple cache lines.
      i.e from the start address to the end of the cache line there isn't
      enough space to store data or the data to read continues to succeeding
      cache lines. To handle this case the request addresses are divided as
      follows:
          1) addrStart to end of first cache-line.
          2) Start of second cache-line to addrEnd.
      Note: addrEnd can be multiple cache-lines ahead of addrStart

      |   cache-line 1   |   cache-line 2   |
      |         |        |        |         |
      |         |        |        |         |
      |         |        |        |         |
      |         V        |        V         |
      |     addrStart    |     addrEnd      |
      |          <--------------->          |
      |             Request size            |
      |------------------|------------------|
  */
  if (requestSpansMultipleCacheLines(addrStart, addrEnd)) {
    std::vector<StandardMem::Request*> reqs;
    uint64_t cacheLineEndAddr =
        nearestCacheLineEnd(addrStart) * cacheLineWidth_;
    uint64_t firstFragmentSize = cacheLineEndAddr - addrStart;
    uint64_t secondFragmentSize = size - firstFragmentSize;
    std::vector<StandardMem::Request*> rvec1 =
        splitAggregatedRequest(aggrReq, addrStart, firstFragmentSize);
    std::vector<StandardMem::Request*> rvec2 =
        splitAggregatedRequest(aggrReq, cacheLineEndAddr, secondFragmentSize);
    reqs.insert(reqs.end(), rvec1.begin(), rvec1.end());
    reqs.insert(reqs.end(), rvec2.begin(), rvec2.end());
    return reqs;
  }
  return splitAggregatedRequest(aggrReq, addrStart, size);
}

std::vector<StandardMem::Request*> SimEngMemInterface::splitAggregatedRequest(
    AggregateWriteRequest* aggrReq, uint64_t addrStart, uint64_t size) {
  std::vector<StandardMem::Request*> requests;
  // Determine the number of cache-lines this store touches.
  int numCacheLinesNeeded = getNumCacheLinesNeeded(size);
  // The write has already been applied functionally to SimpleMem; the SST
  // store exists purely to drive cache occupancy/coherence timing, so the
  // payload bytes are irrelevant. Emit correctly-sized zero payloads at the
  // physical address. This also avoids slicing `aggrReq->data` by a
  // (physical addrStart - virtual target.address) delta, which would be
  // meaningless post-SimOS.
  for (int x = 0; x < numCacheLinesNeeded; x++) {
    uint64_t currReqSize = size;
    if (size > cacheLineWidth_) {
      size -= cacheLineWidth_;
      currReqSize = cacheLineWidth_;
    }
    std::vector<uint8_t> payload(currReqSize, 0);
    StandardMem::Request* writeReq =
        new StandardMem::Write(addrStart, currReqSize, payload);

    addrStart += currReqSize;
    requests.push_back(writeReq);
  }
  return requests;
}

std::vector<StandardMem::Request*> SimEngMemInterface::splitAggregatedRequest(
    AggregateReadRequest* aggrReq, uint64_t addrStart, uint64_t size) {
  std::vector<StandardMem::Request*> requests;
  // Get the number of cache-lines needed to read the data requested by the read
  // request.
  int numCacheLinesNeeded = getNumCacheLinesNeeded(size);

  // Loop used to divide a read request from SimEng based on cache-line size.
  for (int x = 0; x < numCacheLinesNeeded; x++) {
    uint64_t currReqSize = size;
    if (size > cacheLineWidth_) {
      size -= cacheLineWidth_;
      currReqSize = cacheLineWidth_;
    }

    StandardMem::Request* readReq =
        new StandardMem::Read(addrStart, currReqSize);

    // Increase the aggregate count to denote the number SST requests a read
    // request from SimEng was split into.
    aggrReq->aggregateCount_++;
    addrStart += currReqSize;
    requests.push_back(readReq);
    /*
    Insert a key-value pair of SST request id and AggregatedReadRequest
    reference in the aggregation map. These key-value pairs will later be
    used to store read response data recieved from SST. This models a
    many-to-one relation between multiple SST requests and a SimEng read
    request.
    */
    aggregationMap_.insert({readReq->getID(), aggrReq});
  }
  return requests;
}

void SimEngMemInterface::requestRead(const memory::MemoryAccessTarget& target,
                                     uint64_t requestId) {
  dbgReads_++;
  uint64_t size = unsigned(target.size);

  // Service the read functionally and synchronously through the MMU
  // (virtual->physical translation + lazy page-fault handling + SimpleMem).
  // This yields both the correct bytes and the translated physical address.
  simeng::memory::DataPacket resp;
  bool gotResponse = false;
  mmu_->bufferRequest(
      simeng::memory::DataPacket(target.address, size,
                                 simeng::memory::READ_REQUEST, requestId),
      [&](simeng::memory::DataPacket pkt) {
        resp = pkt;
        gotResponse = true;
      });

  // A fault (e.g. wrongly speculated branch producing a wild address) is
  // signalled to the core with an empty RegisterValue, matching the
  // behaviour of FixedLatencyMemoryInterface. No SST request is issued.
  if (!gotResponse || resp.inFault_) {
    dbgFaults_++;
    completedReadRequests_.push_back({target, RegisterValue(), requestId});
    return;
  }

  // resp.address_ holds the physical address; index the SST cache hierarchy
  // by it so cache set/line behaviour is meaningful.
  uint64_t addrStart = resp.address_;
  uint64_t addrEnd = addrStart + size - 1;

  AggregateReadRequest* aggrReq = new AggregateReadRequest(target, requestId);
  // Stash the functionally-correct bytes; SST responses are timing-only.
  aggrReq->funcData_.assign(resp.data_.begin(), resp.data_.end());
  std::vector<StandardMem::Request*> requests =
      makeSSTRequests<AggregateReadRequest>(aggrReq, addrStart, addrEnd, size);
  // SST output data parsed by the testing framework.
  // Format:
  // [SSTSimEng:SSTDebug] MemRead-read-<type=request|response>-<request ID>
  // -cycle-<cycle count>-split-<number of requests>
  if (debug_) {
    std::cout << "[SSTSimEng:SSTDebug] MemRead"
              << "-read-request-" << requestId << "-cycle-" << tickCounter_
              << "-split-" << requests.size() << std::endl;
  }
  for (StandardMem::Request* req : requests) {
    dbgSstSends_++;
    sstMem_->send(req);
  }
}

void SimEngMemInterface::requestWrite(const memory::MemoryAccessTarget& target,
                                      const RegisterValue& data) {
  dbgWrites_++;
  uint64_t size = unsigned(target.size);

  // Apply the write functionally and synchronously through the MMU so the
  // SimpleMem-backed process image stays the correct source of truth for
  // instruction fetch and syscalls. Capture the translated physical address.
  const char* wd = data.getAsVector<char>();
  std::vector<char> wbytes(wd, wd + size);
  simeng::memory::DataPacket resp;
  bool gotResponse = false;
  mmu_->bufferRequest(
      simeng::memory::DataPacket(target.address, size,
                                 simeng::memory::WRITE_REQUEST, 0, wbytes),
      [&](simeng::memory::DataPacket pkt) {
        resp = pkt;
        gotResponse = true;
      });

  // Faulting writes (bad speculative address) carry no architectural effect
  // and need no cache traffic.
  if (!gotResponse || resp.inFault_) return;

  // Issue the store into the SST cache hierarchy at the physical address for
  // timing/coherence-state modelling only; the response is discarded.
  uint64_t addrStart = resp.address_;
  uint64_t addrEnd = addrStart + size - 1;
  AggregateWriteRequest* aggrReq = new AggregateWriteRequest(target, data);
  std::vector<StandardMem::Request*> requests =
      makeSSTRequests<AggregateWriteRequest>(aggrReq, addrStart, addrEnd, size);

  for (StandardMem::Request* req : requests) {
    dbgSstSends_++;
    sstMem_->send(req);
  }
  delete aggrReq;
}

void SimEngMemInterface::tick() {
  tickCounter_++;
  if (debug_ && (tickCounter_ % 5000000) == 0) {
    std::cerr << "[SSTSimEng] memstats: reads=" << dbgReads_
              << " writes=" << dbgWrites_ << " faults=" << dbgFaults_
              << " sstSends=" << dbgSstSends_ << std::endl;
  }
}

void SimEngMemInterface::clearCompletedReads() {
  completedReadRequests_.clear();
}

bool SimEngMemInterface::hasPendingRequests() const {
  return aggregationMap_.size() > 0;
};

const span<memory::MemoryReadResult> SimEngMemInterface::getCompletedReads()
    const {
  return {const_cast<memory::MemoryReadResult*>(completedReadRequests_.data()),
          completedReadRequests_.size()};
};

void SimEngMemInterface::aggregatedReadResponses(
    AggregateReadRequest* aggrReq) {
  if (aggrReq->aggregateCount_ != 0) return;
  // All SST timing responses for this read have now returned, so the core
  // has paid the modelled L1/L2/DRAM latency. Drop the SST request-id ->
  // aggregate bookkeeping; the SST-returned bytes are ignored — the value
  // delivered to the core is the functionally-correct data captured from the
  // MMU/SimpleMem path at request time.
  for (auto itr = aggrReq->responseMap_.begin();
       itr != aggrReq->responseMap_.end(); itr++) {
    aggregationMap_.erase(itr->first);
  }
  uint64_t id = aggrReq->id_;
  if (debug_) {
    std::cout << "[SSTSimEng:SSTDebug] MemRead"
              << "-read-response-" << id << "-cycle-" << tickCounter_
              << "-split-done" << std::endl;
  }

  completedReadRequests_.push_back(
      {aggrReq->target,
       RegisterValue(aggrReq->funcData_.data(),
                     uint16_t(unsigned(aggrReq->target.size))),
       aggrReq->id_});

  // Cleanup
  aggrReq->responseMap_.clear();
  delete aggrReq;
}

void SimEngMemInterface::SimEngMemHandlers::handle(
    StandardMem::WriteResp* rsp) {
  delete rsp;
}

void SimEngMemInterface::SimEngMemHandlers::handle(StandardMem::ReadResp* rsp) {
  uint64_t id = rsp->getID();
  auto data = rsp->data;
  delete rsp;

  // Upon recieving a response from SST the aggregation_map is used to retrieve
  // the AggregatedReadRequest the recieved SST response is a part of.
  auto itr = memInterface_.aggregationMap_.find(id);
  if (itr == memInterface_.aggregationMap_.end()) return;
  /*
      After succesful retrieval of AggregatedReadRequest from aggregation_map
     the response data is stored inside the AggregatedReadRequest in an ordered
     map. It is neccesary to maintain order in which the orginal read request
     from SimEng was split into otherwise garbage values will be obtained upon
     merging. An ordered map is used here because SST::StandardMem::Request ids
     are generated using an atomic incrementing couter. Reference -
     "interfaces/stdMem.(hh/cc)" (SST-Core)
  */
  SimEngMemInterface::AggregateReadRequest* aggrReq = itr->second;
  aggrReq->responseMap_.insert({id, data});
  /*
      Decrement aggregateCount as we keep on recieving responses from SST.
      If all responses have been recieved aggregate all responses and send
      data back to SimEng.
  */
  if (--aggrReq->aggregateCount_ <= 0) {
    memInterface_.aggregatedReadResponses(aggrReq);
  }
}

int SimEngMemInterface::getNumCacheLinesNeeded(uint64_t size) const {
  if (size < cacheLineWidth_) return 1;
  if (size % cacheLineWidth_ == 0) return size / cacheLineWidth_;
  return (size / cacheLineWidth_) + 1;
}
bool SimEngMemInterface::unsignedOverflow_(uint64_t a, uint64_t b) const {
  return (a + b) < a || (a + b) < b;
};
bool SimEngMemInterface::requestSpansMultipleCacheLines(
    uint64_t addrStart, uint64_t addrEnd) const {
  uint64_t lineDiff =
      (addrEnd / cacheLineWidth_) - (addrStart / cacheLineWidth_);
  return lineDiff > 0;
};
uint64_t SimEngMemInterface::nearestCacheLineEnd(uint64_t addrStart) const {
  return (addrStart / cacheLineWidth_) + 1;
};