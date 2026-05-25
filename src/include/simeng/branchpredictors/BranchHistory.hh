#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>

namespace simeng {
/** A class for storing a branch history.  Needed for cases where a branch
 * history of more than 64 bits is required.  This class makes it easier to
 * access and manipulate large branch histories, as are needed in
 * sophisticated branch predictors.
 *
 * The bits of the branch history are stored in an array of uint64_t values,
 * and their access/manipulation is facilitated by the public functions. */

class BranchHistory {
 public:
  BranchHistory(uint64_t size) : size_(size) {
    history_ = std::make_unique<uint64_t[]>(size_);
  }

  ~BranchHistory() {};

  /** Returns the 'numBits' most recent bits of the branch history.  Maximum
   * number of bits returnable is 64 to allow it to be provided in a 64-bit
   * integer. */
  uint64_t getHistory(uint8_t numBits) {
    assert(numBits <= 64 && "Cannot get more than 64 bits without rolling");
    assert(numBits <= size_ &&
           "Cannot get more bits of branch history than "
           "the size of the history");
    return (history_[0] & ((1ull << numBits) - 1));
  }

  /** Returns 'numBits' of the global history folded over on itself to get a
   * value of size 'length'.  The global history is folded by partitioning
   * the requested bit window into non-overlapping chunks of 'length' bits
   * and XOR-combining them — the standard PPM/TAGE folded-history hash.
   *
   * BUG FIX 2026-05-25: previous implementation had multiple defects —
   * shifts by ≥64 bits (UB), a "leftover bits" branch that XORed garbage,
   * `1 << length` trim mask (UB / signed-int when length≥31), and a stride
   * that didn't actually partition the window. Rewrote against TAGE
   * reference. (See memory: tage-fixes-2026-05-24 — the earlier rewrite
   * attempt was reverted because CM IPC dropped, but CM is currently
   * inflated by the Fixed-L1 artifact and the standard fold is the
   * correct algorithm; pending-actions-2026-05-25 Action 3.) */
  uint64_t getFolded(uint8_t numBits, uint8_t length) {
    assert(numBits <= size_ &&
           "Cannot get more bits of branch history than "
           "the size of the history");
    assert(length > 0 && length <= 64 && "fold length must be in (0,64]");
    if (numBits == 0) return 0;

    const uint64_t lengthMask =
        (length == 64) ? ~0ull : ((1ull << length) - 1);
    uint64_t output = 0;

    for (uint64_t i = 0; i < numBits; i += length) {
      uint64_t chunkBits = std::min<uint64_t>(length, numBits - i);
      uint64_t word = i / 64;
      uint64_t bit = i % 64;
      uint64_t chunk;
      if (bit + chunkBits <= 64) {
        // Single-word chunk
        uint64_t mask =
            (chunkBits == 64) ? ~0ull : ((1ull << chunkBits) - 1);
        chunk = (history_[word] >> bit) & mask;
      } else {
        // Chunk straddles a uint64_t boundary
        uint64_t lo = history_[word] >> bit;
        uint64_t hi = history_[word + 1] << (64 - bit);
        uint64_t mask =
            (chunkBits == 64) ? ~0ull : ((1ull << chunkBits) - 1);
        chunk = (lo | hi) & mask;
      }
      output ^= chunk;
    }

    return output & lengthMask;
  }

  /** Adds a branch outcome ('isTaken') to the global history */
  void addHistory(bool isTaken) {
    for (int8_t i = size_ / 64; i >= 0; i--) {
      history_[i] <<= 1;
      if (i == 0) {
        history_[i] |= ((isTaken) ? 1 : 0);
      } else {
        history_[i] |= (history_[i - 1] & 0x80000000) >> 63;
      }
    }
  }

  /** Updates the state of a branch that has already been added to the global
   * history at 'position', where 'position' is 0-indexed and starts from the
   * most recent history.  I.e., to update the most recently added branch
   * outcome, 'position' would be 0.
   * */
  void updateHistory(bool isTaken, uint64_t position) {
    if (position < size_) {
      uint8_t vectIndex = position / 64;
      uint8_t bitIndex = position % 64;
      bool currentlyTaken = ((history_[vectIndex] & (1ull << bitIndex)) != 0);
      if (currentlyTaken != isTaken) {
        history_[vectIndex] ^= (1ull << bitIndex);
      }
    }
  }

  /** Removes the most recently added branch from the history */
  void rollBack() {
    for (uint8_t i = 0; i <= (size_ / 64); i++) {
      history_[i] >>= 1;
      if (i < (size_ / 64)) {
        history_[i] |= (history_[i + 1] & 1ull) << 63;
      }
    }
  }

 private:
  /** The number of bits of branch history stored in this branch history */
  uint64_t size_;

  /** An array containing the bits of the branch history.  The bits are
   * arranged such that the most recent branches are stored in uint64_t at
   * index 0 of the vector, then the next most recent at index 1 and so forth.
   * Within each uint64_t, the most recent branches are recorded in the
   * least-significant bits. */
  std::unique_ptr<uint64_t[]> history_;
};

}  // namespace simeng