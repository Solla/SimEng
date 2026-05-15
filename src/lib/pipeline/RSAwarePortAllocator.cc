#include "simeng/pipeline/RSAwarePortAllocator.hh"

#include <cassert>
#include <cstdint>
#include <vector>

namespace simeng {
namespace pipeline {

RSAwarePortAllocator::RSAwarePortAllocator(
    const std::vector<std::vector<uint16_t>>& portArrangement,
    std::vector<std::pair<uint16_t, uint64_t>> rsArrangement)
    : weights(portArrangement.size(), 0), rsArrangement_(rsArrangement) {}

uint16_t RSAwarePortAllocator::allocate(const std::vector<uint16_t>& ports) {
  assert(ports.size() &&
         "No supported ports supplied; cannot allocate from a empty set");
  bool foundPort = false;
  uint16_t bestPort = 0;
  uint16_t bestWeight = 0xFFFF;

  uint16_t bestRSQueueSize = 0xFFFF;
  // Only used in assertions so produces warning in release mode
  [[maybe_unused]] bool foundRS = false;

  // Update the reference for number of free spaces in the reservation
  // stations
  rsFreeSpaces.clear();
  rsSizes_(rsFreeSpaces);

  for (const auto& portIndex : ports) {
    auto rsIndex = rsArrangement_[portIndex].first;
    auto rsSize = rsArrangement_[portIndex].second;
    auto rsFreeSpace = rsFreeSpaces[rsIndex];
    auto rsQueueSize = (rsSize - rsFreeSpace);

    if (rsQueueSize < bestRSQueueSize) {
      bestRSQueueSize = rsQueueSize;
      foundRS = true;

      // Search for the lowest-weighted port available
      if (!foundPort || weights[portIndex] < bestWeight) {
        foundPort = true;
        bestWeight = weights[portIndex];
        bestPort = portIndex;
      }
    }
  }

  assert(foundPort && foundRS && "Unsupported group; cannot allocate a port");

  // Increment the weight of the allocated port
  weights[bestPort]++;
  return bestPort;
}

void RSAwarePortAllocator::issued(uint16_t port) {
  assert(weights[port] > 0);
  weights[port]--;
}

void RSAwarePortAllocator::deallocate(uint16_t port) { issued(port); }

void RSAwarePortAllocator::setRSSizeGetter(
    std::function<void(std::vector<uint32_t>&)> rsSizes) {
  rsSizes_ = rsSizes;
}

void RSAwarePortAllocator::tick() {}
}  // namespace pipeline
}  // namespace simeng
