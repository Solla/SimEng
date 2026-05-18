#include "ConfigInit.hh"
#include "gtest/gtest.h"
#include "simeng/OS/SimOS.hh"
#include "simeng/config/SimInfo.hh"

namespace {

// Test that we can create an SimOS object
TEST(OSTest, CreateSimOS) {
  // Generate-Special-Dir is intentionally left at its default (True). This
  // test asserts nothing about special files; explicitly setting it False
  // made ModelConfig validation require a pre-existing Special-File-Dir-Path
  // that does not exist in the test build tree and then std::exit(1), which
  // tore down the whole gtest process mid-suite. Removing the override lets
  // the rest of the suite run.
  // NOTE: this test is still a KNOWN pre-existing failure on the SimOS/
  // paged-MMU branch -- its small Simulation-Memory predates the new image
  // layout ("simulation memory < single process image"). Right-sizing it
  // belongs to the separate SimOS image-layout work, not here.
  simeng::ConfigInit configInit(
      simeng::config::ISA::AArch64,
      R"YAML({Process-Image: {Heap-Size: 10000, Stack-Size: 10000, Mmap-Size: 20000},
              Simulation-Memory: {Size: 100000}})YAML");

  // Create the simulation memory
  const size_t memorySize =
      simeng::config::SimInfo::getConfig()["Simulation-Memory"]["Size"].as<size_t>();
  const std::shared_ptr<simeng::memory::Mem> memory =
      std::make_shared<simeng::memory::SimpleMem>(memorySize);

  // Create the instance of the OS
  simeng::span<char> defaultPrg = simeng::span<char>(
      reinterpret_cast<char*>(simeng::OS::hex_), sizeof(simeng::OS::hex_));
  simeng::OS::SimOS OS = simeng::OS::SimOS(memory, defaultPrg);

  // Check default process created. Initial process TID = 1
  const std::shared_ptr<simeng::OS::Process> proc = OS.getProcess(1);
  EXPECT_GT(proc->getHeapStart(), 0);
  EXPECT_GT(proc->getMmapStart(), proc->getHeapStart());
  EXPECT_GT(proc->getStackStart(), proc->getMmapStart());
  EXPECT_EQ(proc->isValid(), true);
  // Check CPU context
  // PC is always 0 for processes assembled by SimEng
  EXPECT_EQ(proc->context_.pc, 0);
  EXPECT_GT(proc->context_.progByteLen, 0);
  EXPECT_GT(proc->context_.sp, 0);
  EXPECT_GT(proc->context_.regFile.size(), 0);
  // Check Initial Process' state
  EXPECT_EQ(proc->status_, simeng::OS::procStatus::waiting);

  // Check syscallHandler created
  EXPECT_TRUE(OS.getSyscallHandler());

  // Check terminateThread
  OS.terminateThread(1);
  EXPECT_EQ(OS.getNumProcesses(), 0);

  // Check terminateThreadGroup
  uint64_t procTid = OS.createProcess(defaultPrg);
  EXPECT_EQ(procTid, 2);
  OS.terminateThreadGroup(procTid);
  EXPECT_EQ(OS.getNumProcesses(), 0);
}

}  // namespace
