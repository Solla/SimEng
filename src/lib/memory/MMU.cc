#include "simeng/memory/MMU.hh"

#include <algorithm>
#include <vector>

#include "simeng/OS/Constants.hh"
#include "simeng/util/Math.hh"
namespace simeng {
namespace memory {

MMU::MMU(std::shared_ptr<Mem> memory, VAddrTranslator fn, uint64_t tid)
    : memory_(memory), translate_(fn), tid_(tid) {}

void MMU::bufferRequest(
    DataPacket request, sendResponseToMemInterface sendResponse) {
  // Since we don't have a TLB yet, treat every memory request as a TLB miss and
  // consult the page table.
  const uint64_t pageSize = OS::defaults::PAGE_SIZE;

  // A request that straddles a page boundary cannot be served by translating
  // only its start address and accessing `size_` bytes contiguously: demand-
  // allocated page frames are not physically contiguous, so the bytes on the
  // second page would land in the wrong frame (and the real frame stay zero).
  // Detect this and serve each page-portion with its own translation. (size_
  // for real accesses is <= a register/cache-line width, so this is the rare
  // path; the common single-page case below is unchanged.)
  if (request.size_ > 0 &&
      downAlign(request.address_, pageSize) !=
          downAlign(request.address_ + request.size_ - 1, pageSize)) {
    const bool isRead = (request.type_ == READ_REQUEST);
    std::vector<char> readData;
    bool fault = false;
    uint64_t off = 0;
    while (off < request.size_) {
      uint64_t vaddr = request.address_ + off;
      uint64_t pageEnd = downAlign(vaddr, pageSize) + pageSize;
      uint64_t chunk = std::min<uint64_t>(pageEnd - vaddr, request.size_ - off);

      uint64_t paddr = translate_(vaddr, tid_);
      uint64_t fc = simeng::OS::masks::faults::getFaultCode(paddr);
      DataPacket sub;
      if (fc == simeng::OS::masks::faults::pagetable::DATA_ABORT) {
        fault = true;
        break;
      } else if (fc == simeng::OS::masks::faults::pagetable::IGNORED) {
        DataPacket subReq =
            isRead ? DataPacket(vaddr, chunk, request.type_, request.id_)
                   : DataPacket(vaddr, chunk, request.type_, request.id_,
                                std::vector<char>(request.data_.begin() + off,
                                                  request.data_.begin() + off +
                                                      chunk));
        sub = memory_->handleIgnoredRequest(subReq);
      } else {
        DataPacket subReq =
            isRead ? DataPacket(paddr, chunk, request.type_, request.id_)
                   : DataPacket(paddr, chunk, request.type_, request.id_,
                                std::vector<char>(request.data_.begin() + off,
                                                  request.data_.begin() + off +
                                                      chunk));
        sub = memory_->requestAccess(subReq);
      }
      if (isRead)
        readData.insert(readData.end(), sub.data_.begin(), sub.data_.end());
      off += chunk;
    }

    DataPacket pkt;
    if (fault) {
      pkt = DataPacket(true);
    } else if (isRead) {
      pkt = request.makeIntoReadResponse(readData);
    } else {
      pkt = request.makeIntoWriteResponse();
    }
    if (!(sendResponse == nullptr)) sendResponse(pkt);
    return;
  }

  uint64_t paddr = translate_(request.address_, tid_);
  uint64_t faultCode = simeng::OS::masks::faults::getFaultCode(paddr);
  DataPacket pkt;

  if (faultCode == simeng::OS::masks::faults::pagetable::DATA_ABORT) {
    pkt = DataPacket(true);
  } else if (faultCode == simeng::OS::masks::faults::pagetable::IGNORED) {
    pkt = memory_->handleIgnoredRequest(request);
  } else {
    request.address_ = paddr;
    pkt = memory_->requestAccess(request);
  }
  if (!(sendResponse == nullptr)) {
    sendResponse(pkt);
  }
}

void MMU::setTid(uint64_t tid) { tid_ = tid; }

}  // namespace memory
}  // namespace simeng
