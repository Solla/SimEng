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

  // Select port by lexicographic (rsQueueSize, perPortInFlightWeight). The
  // outer rsQueueSize chooses the least-loaded RS reachable; the inner weight
  // tie-breaks within an RS so per-port in-flight counts are actually
  // balanced. Without this tie-break (formerly guarded by strict-less on
  // rsQueueSize), every uop in a multi-port single-RS group (e.g. all INT
  // uops in RS1) was steered to the first port listed — throttling issue
  // throughput to that one port and starving the others.
  for (const auto& portIndex : ports) {
    auto rsIndex = rsArrangement_[portIndex].first;
    auto rsSize = rsArrangement_[portIndex].second;
    auto rsFreeSpace = rsFreeSpaces[rsIndex];
    auto rsQueueSize = (rsSize - rsFreeSpace);

    bool better = !foundPort || rsQueueSize < bestRSQueueSize ||
                  (rsQueueSize == bestRSQueueSize &&
                   weights[portIndex] < bestWeight);
    if (better) {
      foundPort = true;
      foundRS = true;
      bestRSQueueSize = rsQueueSize;
      bestWeight = weights[portIndex];
      bestPort = portIndex;
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
