#include "simeng/memory/FixedLatencyMemoryInterface.hh"

#include <iostream>

namespace simeng {

namespace memory {

FixedLatencyMemoryInterface::FixedLatencyMemoryInterface(char* memory,
                                                         size_t size,
                                                         uint16_t latency,
                                                         std::function<uint64_t(uint64_t, uint64_t)> vaddrTranslator)
    : memory_(memory),
      size_(size),
      vaddrTranslator_(vaddrTranslator),
      latency_(latency) {}

void FixedLatencyMemoryInterface::tick() {
  tickCounter_++;

  while (pendingRequests_.size() > 0) {
    const auto& request = pendingRequests_.front();

    if (request.readyAt > tickCounter_) {
      // Head of queue isn't ready yet; end cycle
      break;
    }

    const auto& target = request.target;

    if (request.write) {
      // Write: write data directly to memory
      uint64_t paddr = target.address;
      if (vaddrTranslator_) {
        paddr = vaddrTranslator_(target.address, 0); // TID is 0
      }

      if (paddr + target.size > size_) {
        std::cerr << "[SimEng:FixedLatencyMemoryInterface] Attempted to write "
                     "beyond memory limit."
                  << std::endl;
        exit(1);
      }

      auto ptr = memory_ + paddr;
      // Copy the data from the RegisterValue to memory
      memcpy(ptr, request.data.getAsVector<char>(), target.size);
    } else {
      // Read: read data into `completedReads`
      uint64_t paddr = target.address;
      if (vaddrTranslator_) {
        paddr = vaddrTranslator_(target.address, 0); // TID is 0
      }

      if (paddr + target.size > size_ ||
          unsignedOverflow_(paddr, target.size)) {
        // Read outside of memory; return an invalid value to signal a fault
        completedReads_.push_back({target, RegisterValue(), request.requestId});
      } else {
        const char* ptr = memory_ + paddr;

        // Copy the data at the requested memory address into a RegisterValue
        completedReads_.push_back(
            {target, RegisterValue(ptr, target.size), request.requestId});
      }
    }

    // Remove the request from the queue
    pendingRequests_.pop();
  }
}

void FixedLatencyMemoryInterface::requestRead(const MemoryAccessTarget& target,
                                              uint64_t requestId) {
  pendingRequests_.push({target, tickCounter_ + latency_, requestId});
}

void FixedLatencyMemoryInterface::requestWrite(const MemoryAccessTarget& target,
                                               const RegisterValue& data) {
  pendingRequests_.push({target, data, tickCounter_ + latency_});
}

const span<MemoryReadResult> FixedLatencyMemoryInterface::getCompletedReads()
    const {
  return {const_cast<MemoryReadResult*>(completedReads_.data()),
          completedReads_.size()};
}

void FixedLatencyMemoryInterface::clearCompletedReads() {
  completedReads_.clear();
}

bool FixedLatencyMemoryInterface::hasPendingRequests() const {
  return !pendingRequests_.empty();
}

}  // namespace memory
}  // namespace simeng
