#pragma once

#include <vector>

#include "simeng/memory/MemoryInterface.hh"
#include "simeng/memory/MMU.hh"

namespace simeng {
namespace memory {

/** A memory interface to a flat memory system. */
class FlatMemoryInterface : public MemoryInterface {
 public:
  FlatMemoryInterface(std::shared_ptr<MMU> mmu);

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
  std::vector<MemoryReadResult> completedReads_;
};

}  // namespace memory
}  // namespace simeng
