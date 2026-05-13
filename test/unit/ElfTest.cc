#include "gmock/gmock.h"
#include "simeng/Elf.hh"
#include "simeng/version.hh"

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Return;

namespace simeng {

class ElfTest : public testing::Test {
 public:
  ElfTest() {}

 protected:
  const std::string knownElfFilePath =
      SIMENG_SOURCE_DIR "/test/unit/data/stream-aarch64.elf";

  const uint64_t known_entryPoint = 4206008;
  const uint16_t known_e_phentsize = 56;
  const uint16_t known_e_phnum = 6;
  const uint64_t known_phdrTableAddress = 64;  // Offset in file, not virtual address
  const uint64_t known_processImageSize = 5040480;

};

// Test that a valid ELF file can be created
TEST_F(ElfTest, validElf) {
  Elf elf(knownElfFilePath);

  EXPECT_TRUE(elf.isValid());
  auto exec = elf.getExecutable();
  ASSERT_NE(exec, nullptr);
  EXPECT_EQ(exec->elf_header.e_entry, known_entryPoint);
  EXPECT_EQ(exec->elf_header.e_phentsize, known_e_phentsize);
  EXPECT_EQ(exec->elf_header.e_phnum, known_e_phnum);
  EXPECT_EQ(exec->elf_header.e_phoff, known_phdrTableAddress);
}

// Test that wrong filepath results in invalid ELF
TEST_F(ElfTest, invalidElf) {
  Elf elf(SIMENG_SOURCE_DIR "/test/bogus_file_path___--__--__");
  EXPECT_FALSE(elf.isValid());
}

// Test that non-ELF file is not accepted
TEST_F(ElfTest, nonElf) {
  testing::internal::CaptureStderr();
  Elf elf(SIMENG_SOURCE_DIR "/test/unit/ElfTest.cc");
  EXPECT_FALSE(elf.isValid());
  EXPECT_THAT(testing::internal::GetCapturedStderr(),
              HasSubstr("[SimEng:Elf] Elf magic does not match"));
}

// Check that 32-bit ELF is not accepted
TEST_F(ElfTest, format32Elf) {
  testing::internal::CaptureStderr();
  Elf elf(SIMENG_SOURCE_DIR "/test/unit/data/stream.rv32ima.elf");
  EXPECT_FALSE(elf.isValid());
  EXPECT_THAT(
      testing::internal::GetCapturedStderr(),
      HasSubstr("[SimEng:Elf] Unsupported architecture detected in Elf"));
}

}  // namespace simeng