#pragma once

#include "gmock/gmock.h"
#include "simeng/Core.hh"
#include "simeng/OS/SyscallHandler.hh"

namespace simeng {

/** Mock implementation of the `Core` interface. */
class MockCore : public Core {
 public:
  MockCore() {}
  MOCK_METHOD0(tick, void());
  MOCK_METHOD0(getStatus, CoreStatus());
  MOCK_METHOD1(setStatus, void(CoreStatus));
  MOCK_CONST_METHOD0(getCurrentTID, uint64_t());
  MOCK_CONST_METHOD0(getCoreId, uint64_t());
  MOCK_CONST_METHOD0(getArchitecturalRegisterFileSet,
                     const ArchitecturalRegisterFileSet&());
  MOCK_CONST_METHOD1(sendSyscall, void(OS::SyscallInfo));
  MOCK_CONST_METHOD1(receiveSyscallResult, void(const OS::SyscallResult));
  MOCK_CONST_METHOD0(getInstructionsRetiredCount, uint64_t());
  MOCK_CONST_METHOD0(getStats, std::map<std::string, std::string>());
  MOCK_METHOD1(schedule, void(simeng::OS::cpuContext));
  MOCK_METHOD0(interrupt, bool());
  MOCK_CONST_METHOD0(getCurrentProcTicks, uint64_t());
  MOCK_CONST_METHOD0(getCurrentContext, simeng::OS::cpuContext());
};

}  // namespace simeng
