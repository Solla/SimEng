# Add object library for base regression test fixture class
add_library(regression-test-base OBJECT RegressionTest.cc RegressionTest.hh)

# Include and link to libsimeng
target_include_directories(regression-test-base
                           PUBLIC ${PROJECT_SOURCE_DIR}/src/lib)
target_link_libraries(regression-test-base libsimeng)

# Link to gtest
target_link_libraries(regression-test-base gtest_main)

# Add LLVM includes
target_include_directories(regression-test-base PUBLIC ${LLVM_INCLUDE_DIRS})

# Link to LLVM libraries (only include parsers for enabled architectures)
set(LLVM_ASM_PARSERS object)
if(NOT DEFINED SIMENG_ARCH OR SIMENG_ARCH STREQUAL "AArch64" OR SIMENG_ARCH STREQUAL "")
  list(APPEND LLVM_ASM_PARSERS aarch64asmparser)
endif()
if(NOT DEFINED SIMENG_ARCH OR SIMENG_ARCH STREQUAL "RISCV" OR SIMENG_ARCH STREQUAL "")
  list(APPEND LLVM_ASM_PARSERS riscvasmparser)
endif()
llvm_map_components_to_libnames(LLVM_LIBS ${LLVM_ASM_PARSERS})
target_link_libraries(regression-test-base ${LLVM_LIBS})

# Add regression test directories for each architecture
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(aarch64)
if(NOT DEFINED SIMENG_ARCH OR SIMENG_ARCH STREQUAL "RISCV" OR SIMENG_ARCH STREQUAL "")
  add_subdirectory(riscv)
endif()

# Link to yaml-cpp libraries
target_link_libraries(regression-test-base yaml-cpp)
