#pragma once

#include <memory>
#include <functional>

#include "simeng/config/yaml/ryml.hh"
#include "simeng/Elf.hh"
#include "simeng/config/SimInfo.hh"
#include "simeng/kernel/PageTable.hh"
#include "simeng/kernel/MemRegion.hh"

namespace simeng {
namespace kernel {

namespace auxVec {
// Labels for the entries in the auxiliary vector
enum labels {
  AT_NULL = 0,       // End of vector
  AT_IGNORE = 1,     // Entry should be ignored
  AT_EXECFD = 2,     // File descriptor of program
  AT_PHDR = 3,       // Program headers for program
  AT_PHENT = 4,      // Size of program header entry
  AT_PHNUM = 5,      // Number of program headers
  AT_PAGESZ = 6,     // System page size
  AT_BASE = 7,       // Base address of interpreter
  AT_FLAGS = 8,      // Flags
  AT_ENTRY = 9,      // Entry point of program
  AT_NOTELF = 10,    // Program is not ELF
  AT_UID = 11,       // Real uid
  AT_EUID = 12,      // Effective uid
  AT_GID = 13,       // Real gid
  AT_EGID = 14,      // Effective gid
  AT_PLATFORM = 15,  // String identifying CPU for optimizations
  AT_HWCAP = 16,     // Arch dependent hints at CPU capabilities
  AT_CLKTCK = 17,    // Frequency at which times() increments
  AT_SECURE = 23,    // Boolean, if nonzero, the binary should be treated as a secure binary
  AT_RANDOM = 25,    // Address of 16 random bytes
  AT_HWCAP2 = 26,    // Arch dependent hints at CPU capabilities
  AT_EXECFN = 31     // Filename of executable
};
}  // namespace auxVec

/** Align `address` to an `alignTo`-byte boundary by rounding up to the nearest
 * multiple. */
uint64_t alignToBoundary(uint64_t value, uint64_t boundary);

class Linux; // forward declare

/** The initial state of a Linux process, constructed from a binary executable.
 */
class LinuxProcess {
 public:
  /** Construct a Linux process from a vector of command-line arguments.
   *
   * The first argument is a path to an executable ELF file. */
  LinuxProcess(const std::vector<std::string>& commandLine,
               Linux& os,
               ryml::ConstNodeRef config = config::SimInfo::getConfig());

  /** Construct a Linux process from region of instruction memory, with the
   * entry point fixed at 0 and source directory set to the default programs'.
   * For use in test suites. */
  LinuxProcess(span<const uint8_t> instructions,
               Linux& os,
               ryml::ConstNodeRef config = config::SimInfo::getConfig());

  ~LinuxProcess();

  /** Get the address of the start of the heap region. */
  uint64_t getHeapStart() const;

  /** Get the address of the bottom of the stack. */
  uint64_t getStackStart() const;

  /** Get the address of the start of the mmap region. */
  uint64_t getMmapStart() const;

  /** Get the page size. */
  uint64_t getPageSize() const;

  /** Get the entry point. */
  uint64_t getEntryPoint() const;

  /** This method returns the initial stack pointer.*/
  uint64_t getInitialStackPointer() const;
 
  /** This method returns the process's MemRegion. */
  MemRegion& getMemRegion() { return memRegion_; }
 
  /** This method translates a virtual address to a physical address. */
  std::string getPath() const;

  /** Check whether the process image was created successfully. */
  bool isValid() const;

  /** Translate a virtual address using the process's page table. */
  uint64_t translate(uint64_t vaddr) const;

  /** Handle a page fault for a given virtual address. */
  uint64_t handlePageFault(uint64_t vaddr);

 private:
  /** The size of the stack, in bytes. */
  const uint64_t STACK_SIZE;

  /** The space to reserve for the heap, in bytes. */
  const uint64_t HEAP_SIZE;

  /** Create and populate the initial process stack. */
  uint64_t createStack();

  /** The entry point of the process. */
  uint64_t entryPoint_ = 0;

  /** Program header table virtual address */
  uint64_t progHeaderTableAddress_ = 0;

  /** Number of program headers */
  uint64_t numProgHeaders_ = 0;

  /** Size of program header entry */
  uint64_t progHeaderEntSize_ = 0;

  /** The page size of the process memory. */
  const uint64_t pageSize_ = 4096;

  /** The address of the head/top of the stack */
  uint64_t stackPointer_;

  /** The process command and its arguments. */
  std::vector<std::string> commandLine_;

  /** Whether the process image was created successfully. */
  bool isValid_ = false;

  /** Shared pointer to the Page Table. */
  std::shared_ptr<PageTable> pageTable_;

  /** MemRegion for the process. */
  MemRegion memRegion_;

  /** Reference to OS for page frame allocation and memory writes. */
  Linux& os_;

  /** Is it dynamically linked? */
  bool isDynamic_ = false;

  /** Interpreter entry point. */
  uint64_t interpEntryPoint_ = 0;

  /** Mmap start address. */
  uint64_t mmapStart_ = 0;

  /** Heap start address. */
  uint64_t heapStart_ = 0;

  /** Interpreter load address. */
  uint64_t interpLoadAddr_ = 0;

  /** ELF entry point. */
  uint64_t elfEntryPoint_ = 0;
};

}  // namespace kernel
}  // namespace simeng
