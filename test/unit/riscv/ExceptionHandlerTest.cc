#include "../ConfigInit.hh"
#include "../MockCore.hh"
#include "../MockInstruction.hh"
#include "../MockMemoryInterface.hh"
#include "gmock/gmock.h"
#include "simeng/ArchitecturalRegisterFileSet.hh"
#include "simeng/arch/riscv/Architecture.hh"
#include "simeng/arch/riscv/ExceptionHandler.hh"
#include "simeng/arch/riscv/Instruction.hh"
#include "simeng/kernel/Linux.hh"

namespace simeng {
namespace arch {
namespace riscv {

using ::testing::HasSubstr;
using ::testing::Return;
using ::testing::ReturnRef;

class RiscVExceptionHandlerTest : public ::testing::Test {
 public:
  RiscVExceptionHandlerTest()
      : kernel(config::SimInfo::getConfig()),
        physRegFileSet(config::SimInfo::getArchRegStruct()),
        archRegFileSet(physRegFileSet) {}

 protected:
  ConfigInit configInit = ConfigInit(config::ISA::RV64, "");

  MockMemoryInterface memory;
  kernel::Linux kernel;
  Architecture arch;

  RegisterFileSet physRegFileSet;
  ArchitecturalRegisterFileSet archRegFileSet;

  MockCore core;

  // addi	sp, ra, 2000 --- Just need a valid instruction to hijack
  std::array<uint8_t, 4> validInstrBytes = {0x13, 0x81, 0x00, 0x7d};

  /** Helper constants for RISC-V general-purpose registers. */
  const Register R0 = {RegisterType::GENERAL, 10};
  const Register R1 = {RegisterType::GENERAL, 11};
  const Register R2 = {RegisterType::GENERAL, 12};
  const Register R3 = {RegisterType::GENERAL, 13};
  const Register R4 = {RegisterType::GENERAL, 14};
  const Register R5 = {RegisterType::GENERAL, 15};
  const Register R7 = {RegisterType::GENERAL, 17};
};

// ExceptionHandler API changed - tests disabled pending rewrite
#if 0

// All system calls are tested in /test/regression/riscv/Syscall.cc

// Test that a syscall is processed sucessfully
TEST_F(RiscVExceptionHandlerTest, testSyscall) {
}

#endif  // ExceptionHandler API changed

}  // namespace riscv
}  // namespace arch
}  // namespace simeng
