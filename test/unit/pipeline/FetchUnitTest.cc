#include "../MockArchitecture.hh"
#include "../MockBranchPredictor.hh"
#include "../MockInstruction.hh"
#include "../MockMemoryInterface.hh"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "simeng/Instruction.hh"
#include "simeng/arch/Architecture.hh"
#include "simeng/pipeline/FetchUnit.hh"
#include "simeng/pipeline/PipelineBuffer.hh"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Field;
using ::testing::Return;
using ::testing::SetArgReferee;

namespace simeng {
namespace pipeline {

class PipelineFetchUnitTest : public testing::Test {
 public:
  PipelineFetchUnitTest()
      : output(1, {}),
        fetchBuffer({{0, 16}, 0, 0}),
        completedReads(&fetchBuffer, 1),
        fetchUnit(output, memory, 16, isa, predictor),
        uop(new MockInstruction),
        uopPtr(uop) {
    uopPtr->setInstructionAddress(0);
    fetchUnit.setProgramLength(1024);
    fetchUnit.updatePC(0);
  }

 protected:
  PipelineBuffer<MacroOp> output;
  MockMemoryInterface memory;
  MockArchitecture isa;
  MockBranchPredictor predictor;

  MemoryReadResult fetchBuffer;
  span<MemoryReadResult> completedReads;

  FetchUnit fetchUnit;

  MockInstruction* uop;
  std::shared_ptr<Instruction> uopPtr;
};

// Tests that ticking a fetch unit attempts to predecode from the correct
// program counter and generates output correctly.
TEST_F(PipelineFetchUnitTest, Tick) {
  MacroOp macroOp = {uopPtr};

  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(completedReads));

  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(4));

  // Set the output parameter to a 1-wide macro-op
  EXPECT_CALL(isa, predecode(_, _, 0, _))
      .WillOnce(DoAll(SetArgReferee<3>(macroOp), Return(4)));

  fetchUnit.tick();

  // Verify that the macro-op was pushed to the output
  EXPECT_EQ(output.getTailSlots()[0].size(), 1);
}

// Tests that ticking a fetch unit does nothing if the output has stalled
TEST_F(PipelineFetchUnitTest, TickStalled) {
  output.stall(true);

  // Anticipate testing instruction type; return true for branch
  ON_CALL(*uop, isBranch()).WillByDefault(Return(true));

  EXPECT_CALL(isa, predecode(_, _, _, _)).Times(0);

  EXPECT_CALL(predictor, predict(_, _, _)).Times(0);

  fetchUnit.tick();

  // Verify that nothing was pushed to the output
  EXPECT_EQ(output.getTailSlots()[0].size(), 0);
}

// Tests that the fetch unit will handle instructions that straddle fetch block
// boundaries by automatically requesting the next block of data.
TEST_F(PipelineFetchUnitTest, FetchUnaligned) {
  MacroOp macroOp = {uopPtr};
  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(4));
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(completedReads));

  // Set PC to 14, so there will not be enough data to start decoding
  EXPECT_CALL(isa, predecode(_, _, _, _)).Times(0);
  fetchUnit.updatePC(14);
  fetchUnit.tick();

  // Expect a block starting at address 16 to be requested when we fetch again
  EXPECT_CALL(memory, requestRead(Field(&MemoryAccessTarget::address, 16), _))
      .Times(1);
  fetchUnit.requestFromPC();

  // Tick again, expecting that decoding will now resume
  MemoryReadResult nextBlockValue = {{16, 16}, 0, 1};
  span<MemoryReadResult> nextBlock = {&nextBlockValue, 1};
  EXPECT_CALL(memory, getCompletedReads()).WillOnce(Return(nextBlock));
  EXPECT_CALL(isa, predecode(_, _, _, _))
      .WillOnce(DoAll(SetArgReferee<3>(macroOp), Return(4)));
  fetchUnit.tick();
}

// Test that hasHalted() correctly detects when PC reaches end of program
TEST_F(PipelineFetchUnitTest, halted) {
  // Initially, should not be halted
  EXPECT_FALSE(fetchUnit.hasHalted());

  // Tick once - still shouldn't halt with initial PC
  fetchUnit.tick();
  EXPECT_FALSE(fetchUnit.hasHalted());

  // Update PC to >= programByteLength (1024) - should now be halted
  fetchUnit.updatePC(1024);
  EXPECT_TRUE(fetchUnit.hasHalted());

  // Update PC to just before end - should no longer be halted
  fetchUnit.updatePC(1008);
  EXPECT_FALSE(fetchUnit.hasHalted());

  // Set up mock data for fetch
  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(4));
  MacroOp mOp = {uopPtr};
  MemoryReadResult memReadResult = {{1008, 16}, RegisterValue(0xFFFF, 16), 1};
  span<memory::MemoryReadResult> nextBlock = {&memReadResult, 1};
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(nextBlock));
  ON_CALL(isa, predecode(_, _, _, _))
      .WillByDefault(DoAll(SetArgReferee<3>(mOp), Return(4)));

  // Multiple ticks to process block - should trigger halt after reading past end
  for (int i = 0; i < 4; i++) {
    fetchUnit.tick();
    // After 4 ticks, we've read 4*4=16 bytes from addr 1008, reaching PC 1024
    if (i == 3) {
      EXPECT_TRUE(fetchUnit.hasHalted());
    }
  }
}

// Tests that a min-size instruction (i.e. one max-instruction-size worth of
// data) sitting at the end of a fetch block is predecoded in the same cycle as
// it is fetched, and that the next fetch request targets the following block.
// Adapted from the pre-merge `minSizeInstructionAtEndOfBuffer` test: the
// current FetchUnit no longer exposes a separate getMinInstructionSize() path,
// so this exercises max == min == 4 (AArch64 uniform encoding) at the buffer
// edge.
TEST_F(PipelineFetchUnitTest, minSizeInstructionAtEndOfBuffer) {
  const uint8_t blockSize = 16;
  const uint8_t insnSize = 4;

  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(insnSize));

  // PC at the last 4 bytes of block 0: predecode should run from address 12.
  const uint64_t setPC = blockSize - insnSize;  // 12
  MacroOp mOp = {uopPtr};

  MemoryReadResult firstBlockValue = {{0, blockSize},
                                      RegisterValue(0xFFFF, blockSize), 1};
  span<memory::MemoryReadResult> firstBlock = {&firstBlockValue, 1};
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(firstBlock));
  EXPECT_CALL(isa, predecode(_, _, setPC, _))
      .WillOnce(DoAll(SetArgReferee<3>(mOp), Return(insnSize)));

  fetchUnit.updatePC(setPC);
  fetchUnit.tick();

  // The instruction at the end of the block was predecoded into the output.
  EXPECT_EQ(output.getTailSlots()[0].size(), 1);

  // Buffer should be empty; requesting from PC must target the next block.
  EXPECT_CALL(memory, requestRead(Field(&MemoryAccessTarget::address, 16), _))
      .Times(1);
  fetchUnit.requestFromPC();
}

// Tests that a taken-branch prediction in the middle of a fetch block causes
// the rest of the block to be discarded and that the next fetch targets the
// predicted branch destination.
TEST_F(PipelineFetchUnitTest, fetchTakenBranchMidBlock) {
  const uint8_t blockSize = 16;
  const uint8_t insnSize = 4;
  const uint64_t pc = 16;

  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(insnSize));

  MacroOp mOp = {uopPtr};
  MemoryReadResult memReadResult = {{pc, blockSize},
                                    RegisterValue(0xFFFF, blockSize), 1};
  span<memory::MemoryReadResult> nextBlock = {&memReadResult, 1};
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(nextBlock));
  ON_CALL(isa, predecode(_, _, _, _))
      .WillByDefault(DoAll(SetArgReferee<3>(mOp), Return(insnSize)));

  fetchUnit.updatePC(pc);

  // First tick: instruction at PC=16 is a non-branch; PC advances to 20.
  EXPECT_CALL(*uop, isBranch()).WillOnce(Return(false));
  fetchUnit.tick();
  EXPECT_EQ(fetchUnit.getPC(), 20);

  // Second tick: instruction at PC=20 is a taken branch to address 320.
  const uint64_t target = 320;
  EXPECT_CALL(*uop, isBranch()).WillOnce(Return(true));
  EXPECT_CALL(*uop, getBranchType())
      .WillOnce(Return(BranchType::Unconditional));
  EXPECT_CALL(*uop, getKnownOffset()).WillOnce(Return(target - 20));
  BranchPrediction pred = {true, target};
  EXPECT_CALL(predictor, predict(20, BranchType::Unconditional, target - 20))
      .WillOnce(Return(pred));
  fetchUnit.tick();

  // PC must follow the predicted target, and the next memory request must
  // target the predicted destination block.
  EXPECT_EQ(fetchUnit.getPC(), target);
  EXPECT_CALL(memory,
              requestRead(Field(&MemoryAccessTarget::address, target), _))
      .Times(1);
  fetchUnit.requestFromPC();
}

// Tests that a properly aligned PC (to the fetch block boundary) triggers a
// memory request at that address and that the fetched block is then predecoded.
TEST_F(PipelineFetchUnitTest, fetchAligned) {
  const uint64_t pc = 16;
  const uint8_t blockSize = 16;
  const uint8_t insnSize = 4;

  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(insnSize));

  MemoryAccessTarget target = {pc, blockSize};
  EXPECT_CALL(memory, requestRead(target, _)).Times(1);

  fetchUnit.updatePC(pc);
  fetchUnit.requestFromPC();

  MacroOp mOp = {uopPtr};
  MemoryReadResult memReadResult = {target, RegisterValue(0xFFFF, blockSize),
                                    1};
  span<memory::MemoryReadResult> nextBlock = {&memReadResult, 1};
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(nextBlock));
  EXPECT_CALL(isa, predecode(_, _, pc, _))
      .WillOnce(DoAll(SetArgReferee<3>(mOp), Return(insnSize)));

  fetchUnit.tick();

  EXPECT_EQ(output.getTailSlots()[0].size(), 1);
}

// Wider-output fixture used by the branch-stall test: branchStalls_ in
// FetchUnit::tick() only increments when a taken-branch is predicted in a slot
// before the last (`slot + 1 < output.getWidth()`), so a width-1 output can
// never observe the increment.
class PipelineFetchUnitWideTest : public testing::Test {
 public:
  PipelineFetchUnitWideTest()
      : output(4, {}),
        fetchUnit(output, memory, 16, isa, predictor),
        uop(new MockInstruction),
        uopPtr(uop) {
    uopPtr->setInstructionAddress(0);
    fetchUnit.setProgramLength(1024);
    fetchUnit.updatePC(0);
  }

 protected:
  PipelineBuffer<MacroOp> output;
  MockMemoryInterface memory;
  MockArchitecture isa;
  MockBranchPredictor predictor;

  FetchUnit fetchUnit;

  MockInstruction* uop;
  std::shared_ptr<Instruction> uopPtr;
};

// Tests that a taken-branch predicted in a non-final output slot increments
// the branch-stall counter (adapted from the pre-merge
// `branchesFetchedCountedIncorrectly` test, which relied on a removed
// `getBranchFetchedCount()` getter).
TEST_F(PipelineFetchUnitWideTest, takenBranchInNonFinalSlotStalls) {
  const uint8_t blockSize = 16;
  const uint8_t insnSize = 4;
  const uint64_t pc = 0;

  ON_CALL(isa, getMaxInstructionSize()).WillByDefault(Return(insnSize));

  MacroOp mOp = {uopPtr};
  MemoryReadResult memReadResult = {{pc, blockSize},
                                    RegisterValue(0xFFFF, blockSize), 1};
  span<memory::MemoryReadResult> block = {&memReadResult, 1};
  ON_CALL(memory, getCompletedReads()).WillByDefault(Return(block));
  ON_CALL(isa, predecode(_, _, _, _))
      .WillByDefault(DoAll(SetArgReferee<3>(mOp), Return(insnSize)));

  // First predecoded instruction (slot 0) is a taken branch to 0x40. Because
  // slot 0 < width-1 (=3), branchStalls_ must increment exactly once.
  EXPECT_CALL(*uop, isBranch()).WillOnce(Return(true));
  EXPECT_CALL(*uop, getBranchType())
      .WillOnce(Return(BranchType::Unconditional));
  EXPECT_CALL(*uop, getKnownOffset()).WillOnce(Return(0x40));
  EXPECT_CALL(predictor, predict(0, BranchType::Unconditional, 0x40))
      .WillOnce(Return(BranchPrediction{true, 0x40}));

  EXPECT_EQ(fetchUnit.getBranchStalls(), 0);
  fetchUnit.tick();
  EXPECT_EQ(fetchUnit.getBranchStalls(), 1);
}

}  // namespace pipeline
}  // namespace simeng
