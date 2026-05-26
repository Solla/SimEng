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
  /** Construct a branch history holding `sizeBits` bits. */
  BranchHistory(uint64_t sizeBits) : sizeBits_(sizeBits) {
    // Need ceil(sizeBits/64) words for storage, plus one extra so that
    // addHistory/rollBack can shift a carry bit into index sizeBits_/64
    // without overrunning the buffer.
    history_ = std::make_unique<uint64_t[]>(sizeBits_ / 64 + 1);
  }

  ~BranchHistory() {};

  /** Returns the 'numBits' most recent bits of the branch history.  Maximum
   * number of bits returnable is 64 to allow it to be provided in a 64-bit
   * integer. */
  uint64_t getHistory(uint8_t numBits) {
    assert(numBits <= 64 && "Cannot get more than 64 bits without rolling");
    assert(numBits <= sizeBits_ &&
           "Cannot get more bits of branch history than "
           "the size of the history");
    return (history_[0] & ((1ull << numBits) - 1));
  }

  /** Returns 'numBits' of the global history folded over on itself to get a
   * value of size 'length'.  The global history is folded by partitioning
   * the requested bit window into non-overlapping chunks of 'length' bits
   * and XOR-combining them — the standard PPM/TAGE folded-history hash. */
  uint64_t getFolded(uint8_t numBits, uint8_t length) {
    // PRE-a70911eb version, restored for #2 measurement comparison only.
    assert(numBits <= sizeBits_ &&
           "Cannot get more bits of branch history than "
           "the size of the history");
    uint64_t output = 0;
    uint64_t startIndex = 0;
    uint64_t endIndex = numBits - 1;
    while (startIndex <= numBits) {
      output ^= ((history_[startIndex / 64] >> startIndex) &
                 ((1ull << (numBits - startIndex)) - 1));
      if ((startIndex / 64) == (endIndex / 64)) {
        uint8_t leftOverBits = endIndex % 64;
        output ^= (history_[endIndex / 64] << (numBits - leftOverBits));
      }
      startIndex += length;
      endIndex += length;
    }
    output &= (1 << length) - 1;
    return output;
  }

  /** Adds a branch outcome ('isTaken') to the global history */
  void addHistory(bool isTaken) {
    for (int8_t i = sizeBits_ / 64; i >= 0; i--) {
      history_[i] <<= 1;
      if (i == 0) {
        history_[i] |= ((isTaken) ? 1 : 0);
      } else {
        history_[i] |= (history_[i - 1] & 0x8000000000000000ull) >> 63;
      }
    }
  }

  /** Updates the state of a branch that has already been added to the global
   * history at 'position', where 'position' is 0-indexed and starts from the
   * most recent history.  I.e., to update the most recently added branch
   * outcome, 'position' would be 0.
   * */
  void updateHistory(bool isTaken, uint64_t position) {
    if (position < sizeBits_) {
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
    for (uint8_t i = 0; i <= (sizeBits_ / 64); i++) {
      history_[i] >>= 1;
      if (i < (sizeBits_ / 64)) {
        history_[i] |= (history_[i + 1] & 1ull) << 63;
      }
    }
  }

 private:
  /** The number of bits of branch history stored in this branch history.
   * Renamed from `size_` (2026-05-27) to make it explicit that this counts
   * bits, not uint64_t storage elements. The backing array holds
   * `sizeBits_/64 + 1` words. */
  uint64_t sizeBits_;

  /** An array containing the bits of the branch history.  The bits are
   * arranged such that the most recent branches are stored in uint64_t at
   * index 0 of the vector, then the next most recent at index 1 and so forth.
   * Within each uint64_t, the most recent branches are recorded in the
   * least-significant bits. */
  std::unique_ptr<uint64_t[]> history_;
};

}  // namespace simeng
