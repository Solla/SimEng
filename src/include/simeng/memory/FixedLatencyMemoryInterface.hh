#pragma once

#include <queue>
#include <vector>

#include "simeng/memory/MemoryInterface.hh"
#include "simeng/memory/MMU.hh"

namespace simeng {
namespace memory {

struct FixedLatencyMemoryInterfaceRequest {
  bool write;
  const MemoryAccessTarget target;
  const RegisterValue data;
  uint64_t readyAt;
  uint64_t requestId;

  FixedLatencyMemoryInterfaceRequest(const MemoryAccessTarget& target,
                                     const RegisterValue& data,
                                     uint64_t readyAt)
      : write(true), target(target), data(data), readyAt(readyAt) {}

  FixedLatencyMemoryInterfaceRequest(const MemoryAccessTarget& target,
                                     uint64_t readyAt, uint64_t requestId)
      : write(false), target(target), readyAt(readyAt), requestId(requestId) {}
};

class FixedLatencyMemoryInterface : public MemoryInterface {
 public:
  FixedLatencyMemoryInterface(std::shared_ptr<MMU> mmu, uint16_t latency);

  void requestRead(const MemoryAccessTarget& target,
                   uint64_t requestId = 0) override;
  void requestWrite(const MemoryAccessTarget& target,
                    const RegisterValue& data) override;
  const span<MemoryReadResult> getCompletedReads() const override;
  void clearCompletedReads() override;
  bool hasPendingRequests() const override;
  void tick() override;

 private:
  std::shared_ptr<MMU> mmu_;
  uint16_t latency_;
  std::vector<MemoryReadResult> completedReads_;
  std::queue<FixedLatencyMemoryInterfaceRequest> pendingRequests_;
  uint64_t tickCounter_ = 0;

  bool unsignedOverflow_(uint64_t a, uint64_t b) const {
    return (a + b) < a || (a + b) < b;
  }
};

}  // namespace memory
}  // namespace simeng
