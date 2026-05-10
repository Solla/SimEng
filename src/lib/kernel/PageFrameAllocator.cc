#include "simeng/kernel/PageFrameAllocator.hh"

#include <iostream>

namespace simeng {
namespace kernel {

namespace {
uint64_t upAlign(uint64_t value, uint64_t boundary) {
  auto remainder = value % boundary;
  if (remainder == 0) {
    return value;
  }
  return value + (boundary - remainder);
}
}  // namespace

PageFrameAllocator::PageFrameAllocator(uint64_t maxSize)
    : maxAllocationSize_(maxSize), sizeLeft_(maxSize) {}

uint64_t PageFrameAllocator::allocate(size_t size) {
  size = upAlign(size, PAGE_SIZE);
  if (size > sizeLeft_) {
    std::cerr
        << "[SimEng:PageFrameAllocator] Cannot allocate more page frames! "
           "Increase the {Simulation-Memory:{Size:<size>}} parameter "
           "in the YAML config file used to run the simulation."
        << std::endl;
    std::exit(1);
  }
  uint64_t paddr = nextFreeAddr_;
  sizeLeft_ -= size;
  nextFreeAddr_ += size;
  return paddr;
}

}  // namespace kernel
}  // namespace simeng
