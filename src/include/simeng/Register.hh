#pragma once
#include <cstdint>
#include <ostream>

namespace simeng {

/** A generic register identifier. */
struct Register {
  /** An identifier representing the type of register - e.g. 0 = general, 1 =
   * vector. Used to determine which register file to access. */
  uint8_t type = 0;

  /** A tag identifying the register. May correspond to either physical or
   * architectural register, depending on point of usage. */
  uint16_t tag = 0;

  /** A boolean identifier for whether the creation of this register has been a
   * result of a register renaming scheme. */
  bool renamed = false;

  /** Default Constructor. */
  Register(){};

  /** Constructor for `Register` with a given register file type and register
   * tag. */
  Register(uint8_t type, uint16_t tag) : type(type), tag(tag){};

  /** Constructor for `Register` with a given register file type, register
   * tag, and renamed status. */
  Register(uint8_t type, uint16_t tag, bool renamed)
      : type(type), tag(tag), renamed(renamed){};

  /** Check for equality of two register identifiers. */
  bool operator==(const Register& other) const {
    return (other.type == type && other.tag == tag);
  }

  /** Check for inequality of two register identifiers. */
  bool operator!=(const Register& other) const { return !(other == *this); }

  /** Default copy constructor. */
  Register(const Register& res) = default;

  /** Default move constructor. */
  Register(Register&& res) = default;

  /** Default copy assignment. */
  Register& operator=(const Register& res) = default;

  /** Default move assignment. */
  Register& operator=(Register&& res) = default;
};

std::ostream& operator<<(std::ostream& os, simeng::Register const& reg);

}  // namespace simeng
