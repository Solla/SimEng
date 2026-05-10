#include "simeng/kernel/LinuxProcess.hh"

#include <cassert>
#include <cstring>
#include <iostream>

#include "simeng/kernel/Linux.hh"

namespace simeng {
namespace kernel {

namespace {
uint64_t upAlign(uint64_t value, uint64_t boundary) {
  auto remainder = value % boundary;
  if (remainder == 0) {
    return value;
  }
 
  return value + (boundary - remainder);
}
 
uint64_t downAlign(uint64_t value, uint64_t boundary) {
  auto remainder = value % boundary;
  return value - remainder;
}
 
uint64_t pageOffset(uint64_t value, uint64_t pageSize) {
  return value % pageSize;
}
}  // namespace

LinuxProcess::LinuxProcess(const std::vector<std::string>& commandLine,
                           Linux& os,
                           ryml::ConstNodeRef config)
    : STACK_SIZE(config["Process-Image"]["Stack-Size"].as<uint64_t>()),
      HEAP_SIZE(config["Process-Image"]["Heap-Size"].as<uint64_t>()),
      commandLine_(commandLine),
      os_(os) {
  // Parse ELF file
  assert(commandLine.size() > 0);
  
  std::string interpreterPath = "";
  if (config["Process-Image"].has_child("Dynamic-Linking") && 
      config["Process-Image"]["Dynamic-Linking"].as<bool>()) {
    if (config["Process-Image"].has_child("Interpreter-Path")) {
      interpreterPath = config["Process-Image"]["Interpreter-Path"].as<std::string>();
    }
  }

  Elf elf(commandLine[0], interpreterPath);
  if (!elf.isValid()) {
    return;
  }
  isValid_ = true;

  pageTable_ = std::make_shared<PageTable>();
  std::function<uint64_t(uint64_t, size_t)> unmapFn =
      [this](uint64_t vaddr, size_t size) -> uint64_t {
    uint64_t value = pageTable_->deleteMapping(vaddr, size);
    if (value ==
        (masks::faults::pagetable::FAULT | masks::faults::pagetable::UNMAP)) {
      std::cerr << "[SimEng:LinuxProcess] Mapping doesn't exist for vaddr: " << vaddr
                << " and length: " << size << std::endl;
    }
    return value;
  };

  auto executable = elf.getExecutable();
  auto& elf_ehdr = executable->elf_header;
  auto& elf_phdrs = executable->loadable_phdrs;

  uint64_t min_addr = -1;
  uint64_t max_addr = 0;
  uint64_t phtAddr = 0;

  uint32_t bss = 0;
  uint32_t brk = 0;

  for (auto phdr : elf_phdrs) {
    if ((phdr.p_offset <= elf_ehdr.e_phoff) &&
        (elf_ehdr.e_phoff < phdr.p_offset + phdr.p_filesz)) {
      phtAddr = phdr.p_vaddr + (elf_ehdr.e_phoff - phdr.p_offset);
    }

    min_addr = std::min(min_addr, downAlign(phdr.p_vaddr, 4096));
    max_addr = std::max(max_addr, phdr.p_vaddr + phdr.p_filesz);

    uint32_t temp = phdr.p_vaddr + phdr.p_filesz;
    bss = std::max(bss, temp);

    temp = phdr.p_vaddr + phdr.p_memsz;
    brk = std::max(brk, temp);
  }
  pageTable_->ignoreAddrRange(0, min_addr);

  bss = upAlign(bss, 4096);
  brk = upAlign(brk, 4096);

  uint64_t addr_space_end = 1ULL << 48;
  uint64_t stack_top = addr_space_end;
  uint64_t stack_size = STACK_SIZE;
  uint64_t stack_end = stack_top - stack_size;
  uint64_t mmap_start = stack_top / 4;

  memRegion_ = MemRegion(
      stack_top, stack_end, brk, stack_top, mmap_start, stack_top, 0, unmapFn);

  uint64_t paddr = 0;
  uint64_t taddr = 0;
  auto sendToMem_ = os_.getSendToMem();

  for (auto phdr : elf_phdrs) {
    uint64_t startAddr = downAlign(phdr.p_vaddr, 4096);
    uint64_t endAddrMemSz = upAlign(phdr.p_vaddr + phdr.p_memsz, 4096);
    uint64_t size = endAddrMemSz - startAddr;

    [[maybe_unused]] uint64_t retAddr = memRegion_.mmapRegion(
        startAddr, size, 0, syscalls::mmap::flags::SIMENG_MAP_FIXED,
        HostFileMMap());

    assert(retAddr == startAddr &&
        "Address returned from mmapRegion MAP_FIXED is not the same as supplied arg.");

    paddr = os_.requestPageFrames(size);
    pageTable_->createMapping(startAddr, paddr, size);

    taddr = pageTable_->translate(phdr.p_vaddr);
    sendToMem_(phdr.data, taddr, phdr.p_filesz);

    // Zero out any BSS data
    if (phdr.p_memsz > phdr.p_filesz) {
      std::vector<char> zero(phdr.p_memsz - phdr.p_filesz, 0);
      sendToMem_(zero, taddr + phdr.p_filesz, zero.size());
    }
  }

  // Map the stack and populate the page table.
  memRegion_.mmapRegion(
      stack_top - stack_size, stack_size, 0,
      syscalls::mmap::flags::SIMENG_MAP_FIXED, HostFileMMap());
  paddr = os_.requestPageFrames(stack_size);
  pageTable_->createMapping(stack_top - stack_size, paddr, stack_size);

  // Populate 1 page for the heap
  memRegion_.mmapRegion(
      brk, pageSize_, 0, syscalls::mmap::flags::SIMENG_MAP_FIXED,
      HostFileMMap());
  paddr = os_.requestPageFrames(pageSize_);
  pageTable_->createMapping(brk, paddr, pageSize_);

  auto interpreter = elf.getInterpreter();
  if (interpreter) {
    auto& interp_ehdr = interpreter->elf_header;
    bool addr_not_set = true;
    uint64_t load_addr = 0;
    uint64_t interp_bss = 0;
    uint64_t interp_brk = 0;

    isDynamic_ = true;
    interpEntryPoint_ = 0;

    for (auto& phdr : interpreter->loadable_phdrs) {
      uint64_t vaddr = phdr.p_vaddr;
      int flags = 0;

      if (addr_not_set) {
        load_addr = -vaddr;
      } else {
        flags |= syscalls::mmap::flags::SIMENG_MAP_FIXED;
      }

      vaddr = load_addr + vaddr;
      vaddr = downAlign(vaddr, pageSize_);

      uint64_t size = phdr.p_filesz + pageOffset(phdr.p_vaddr, pageSize_);
      size = upAlign(size, pageSize_);

      uint64_t map_addr =
          memRegion_.mmapRegion(vaddr, size, 0, flags, HostFileMMap());

      if (addr_not_set) {
        load_addr = map_addr - downAlign(vaddr, pageSize_);
        addr_not_set = false;
      }

      interp_bss = std::max(interp_bss, load_addr + phdr.p_vaddr + phdr.p_filesz);
      interp_brk = std::max(interp_brk, load_addr + phdr.p_vaddr + phdr.p_memsz);

      paddr = os_.requestPageFrames(size);
      pageTable_->createMapping(map_addr, paddr, size);

      uint64_t offset = phdr.p_vaddr - downAlign(phdr.p_vaddr, pageSize_);
      taddr = pageTable_->translate(map_addr + offset);
      sendToMem_(phdr.data, taddr, phdr.p_filesz);

      // Zero out any BSS data
      if (phdr.p_memsz > phdr.p_filesz) {
        std::vector<char> zero(phdr.p_memsz - phdr.p_filesz, 0);
        sendToMem_(zero, taddr + phdr.p_filesz, zero.size());
      }
    }

    interpEntryPoint_ = load_addr + interp_ehdr.e_entry;

    if (upAlign(interp_brk, pageSize_) > upAlign(interp_bss, pageSize_)) {
      interp_bss = downAlign(interp_bss, pageSize_);
      memRegion_.mmapRegion(
          interp_bss, interp_brk - interp_bss, 0, syscalls::mmap::flags::SIMENG_MAP_FIXED,
          HostFileMMap());
    }
    interpLoadAddr_ = load_addr;
  }

  progHeaderTableAddress_ = phtAddr;
  progHeaderEntSize_ = elf_ehdr.e_phentsize;
  numProgHeaders_ = elf_ehdr.e_phnum;
  elfEntryPoint_ = elf_ehdr.e_entry;
  entryPoint_ = isDynamic_ ? interpEntryPoint_ : elfEntryPoint_;

  heapStart_ = brk;
  mmapStart_ = mmap_start;

  uint64_t stackPtr = createStack();
  memRegion_.updateStack(stackPtr);

  std::cout << "[SimEng:LinuxProcess] Process Created" << std::endl;
  std::cout << "[SimEng:LinuxProcess]   Entry Point: 0x" << std::hex << entryPoint_ << std::dec << std::endl;
  std::cout << "[SimEng:LinuxProcess]   Stack Pointer: 0x" << std::hex << stackPtr << std::dec << std::endl;
  if (isDynamic_) {
    std::cout << "[SimEng:LinuxProcess]   Interpreter Load Addr: 0x" << std::hex << interpLoadAddr_ << std::dec << std::endl;
  }
}

LinuxProcess::LinuxProcess(span<const uint8_t> instructions,
                           Linux& os,
                           ryml::ConstNodeRef config)
    : STACK_SIZE(config["Process-Image"]["Stack-Size"].as<uint64_t>()),
      HEAP_SIZE(config["Process-Image"]["Heap-Size"].as<uint64_t>()),
      os_(os) {
  // Set program command string to the full path of the default program even
  // though these aren't the instructions being executed
  commandLine_.push_back(SIMENG_SOURCE_DIR "/SimEngDefaultProgram\0");

  isValid_ = true;

  pageTable_ = std::make_shared<PageTable>();
  std::function<uint64_t(uint64_t, size_t)> unmapFn =
      [this](uint64_t vaddr, size_t size) -> uint64_t {
    return pageTable_->deleteMapping(vaddr, size);
  };

  uint64_t addr_space_end = 1ULL << 48;
  uint64_t stack_top = addr_space_end;
  uint64_t stack_size = STACK_SIZE;
  uint64_t stack_end = stack_top - stack_size;
  uint64_t mmap_start = stack_top / 4;

  uint64_t instrSize = upAlign(instructions.size(), pageSize_);
  uint64_t heapStart = upAlign(instrSize, pageSize_);

  memRegion_ = MemRegion(
      stack_top, stack_end, heapStart, stack_top, mmap_start, stack_top, 0, unmapFn);

  uint64_t instrPhyAddr = os_.requestPageFrames(instrSize);
  uint64_t stackPhyAddr = os_.requestPageFrames(stack_size);
  uint64_t heapPhyAddr = os_.requestPageFrames(pageSize_);

  pageTable_->createMapping(0, instrPhyAddr, instrSize);
  pageTable_->createMapping(stack_top - stack_size, stackPhyAddr, stack_size);
  pageTable_->createMapping(heapStart, heapPhyAddr, pageSize_);

  uint64_t taddr = pageTable_->translate(0);
  std::vector<char> data(instructions.begin(), instructions.end());
  os_.getSendToMem()(data, taddr, instructions.size());

  heapStart_ = heapStart;
  mmapStart_ = mmap_start;

  uint64_t stackPtr = createStack();
  memRegion_.updateStack(stackPtr);
}

LinuxProcess::~LinuxProcess() {}

uint64_t LinuxProcess::getHeapStart() const { return heapStart_; }

uint64_t LinuxProcess::getStackStart() const { return memRegion_.getStackStart(); }

uint64_t LinuxProcess::getMmapStart() const { return mmapStart_; }

uint64_t LinuxProcess::getPageSize() const { return pageSize_; }

std::string LinuxProcess::getPath() const { return commandLine_[0]; }

bool LinuxProcess::isValid() const { return isValid_; }

uint64_t LinuxProcess::getEntryPoint() const { return entryPoint_; }

uint64_t LinuxProcess::getInitialStackPointer() const { return stackPointer_; }

uint64_t LinuxProcess::translate(uint64_t vaddr) const {
  return pageTable_->translate(vaddr);
}

uint64_t LinuxProcess::handlePageFault(uint64_t vaddr) {
  std::cout << "[SimEng:LinuxProcess] Page Fault at 0x" << std::hex << vaddr << std::dec << std::endl;
  VirtualMemoryArea vm = memRegion_.getVMAFromAddr(vaddr);
  if (vm.vmSize_ == 0)
    return masks::faults::pagetable::FAULT |
           masks::faults::pagetable::DATA_ABORT;

  uint64_t alignedVAddr = downAlign(vaddr, pageSize_);

  uint64_t paddr = os_.requestPageFrames(pageSize_);
  uint64_t ret = pageTable_->createMapping(alignedVAddr, paddr, pageSize_);
  if (ret & masks::faults::pagetable::FAULT)
    return masks::faults::pagetable::FAULT | masks::faults::pagetable::MAP;
  uint64_t taddr = pageTable_->translate(vaddr);

  bool hasFile = vm.hasFile();
  if (!hasFile) return taddr;

  void* filebuf = vm.getFileBuf();
  uint64_t offset = alignedVAddr - vm.vmStart_;
  size_t writeLen = vm.getFileSize() - (offset);
  writeLen = writeLen > pageSize_ ? pageSize_ : writeLen;

  char* castedFileBuf = static_cast<char*>(filebuf);
  std::vector<char> data(
      castedFileBuf + offset, castedFileBuf + offset + writeLen);
  if (writeLen > 0) {
    os_.getSendToMem()(data, paddr, writeLen);
  }
  return taddr;
}

uint64_t LinuxProcess::createStack() {
  uint64_t stackPointer = memRegion_.getStackStart();
  uint64_t paddr;
  std::vector<uint64_t> initialStackFrame;
  std::vector<uint8_t> stringBytes;

  // Program arguments (argc, argv[])
  initialStackFrame.push_back(commandLine_.size());  // argc
  for (size_t i = 0; i < commandLine_.size(); i++) {
    char* argvi = commandLine_[i].data();
    for (size_t j = 0; j < commandLine_[i].size(); j++) {
      stringBytes.push_back(argvi[j]);
    }
    stringBytes.push_back(0);
  }
  // Environment strings
  std::vector<std::string> envStrings = {"OMP_NUM_THREADS=1"};
  for (std::string& env : envStrings) {
    for (size_t i = 0; i < env.size(); i++) {
      stringBytes.push_back(env.c_str()[i]);
    }
    stringBytes.push_back(0);
  }

  // Random bytes for AT_RANDOM (16 bytes)
  std::vector<uint8_t> randomBytes(16, 0x42); // Not really random but consistent
  stackPointer -= upAlign(randomBytes.size(), 16);
  uint64_t randomPtr = stackPointer;
  paddr = pageTable_->translate(randomPtr);
  std::vector<char> randomData(randomBytes.begin(), randomBytes.end());
  os_.getSendToMem()(randomData, paddr, randomData.size());

  // Executable filename for AT_EXECFN
  std::string execFilename = commandLine_[0];
  stackPointer -= upAlign(execFilename.size() + 1, 16);
  uint64_t execFnPtr = stackPointer;
  paddr = pageTable_->translate(execFnPtr);
  std::vector<char> execFnData(execFilename.begin(), execFilename.end());
  execFnData.push_back(0);
  os_.getSendToMem()(execFnData, paddr, execFnData.size());

  stackPointer -= upAlign(stringBytes.size() + 1, 32);
  uint16_t ptrCount = 1;
  initialStackFrame.push_back(stackPointer);  // argv[0] ptr
  for (size_t i = 0; i < stringBytes.size(); i++) {
    if (ptrCount == commandLine_.size()) {
      initialStackFrame.push_back(0);
      ptrCount++;
    }
    if (i > 0 && stringBytes[i - 1] == 0x0) {
      initialStackFrame.push_back(stackPointer + (i));
      ptrCount++;
    }
  }

  paddr = pageTable_->translate(stackPointer);
  std::vector<char> strData(stringBytes.begin(), stringBytes.end());
  os_.getSendToMem()(strData, paddr, strData.size());

  initialStackFrame.push_back(0);  // null terminator

  initialStackFrame.push_back(auxVec::AT_PAGESZ);  // AT_PAGESZ
  initialStackFrame.push_back(pageSize_);

  initialStackFrame.push_back(auxVec::AT_PHDR);  // AT_PHDR
  initialStackFrame.push_back(progHeaderTableAddress_);

  initialStackFrame.push_back(auxVec::AT_PHENT);  // AT_PHENT
  initialStackFrame.push_back(progHeaderEntSize_);

  initialStackFrame.push_back(auxVec::AT_PHNUM);  // AT_PHNUM
  initialStackFrame.push_back(numProgHeaders_);

  initialStackFrame.push_back(auxVec::AT_BASE);  // AT_BASE
  initialStackFrame.push_back(interpLoadAddr_);

  initialStackFrame.push_back(auxVec::AT_ENTRY);  // AT_ENTRY
  initialStackFrame.push_back(elfEntryPoint_);

  initialStackFrame.push_back(auxVec::AT_RANDOM);  // AT_RANDOM
  initialStackFrame.push_back(randomPtr);

  initialStackFrame.push_back(auxVec::AT_EXECFN);  // AT_EXECFN
  initialStackFrame.push_back(execFnPtr);

  // UID/GID
  initialStackFrame.push_back(auxVec::AT_UID);
  initialStackFrame.push_back(0);
  initialStackFrame.push_back(auxVec::AT_EUID);
  initialStackFrame.push_back(0);
  initialStackFrame.push_back(auxVec::AT_GID);
  initialStackFrame.push_back(0);
  initialStackFrame.push_back(auxVec::AT_EGID);
  initialStackFrame.push_back(0);

  initialStackFrame.push_back(auxVec::AT_SECURE);
  initialStackFrame.push_back(0);

  // HWCAP
  initialStackFrame.push_back(auxVec::AT_HWCAP);
  initialStackFrame.push_back(0xffffffff);

  initialStackFrame.push_back(auxVec::AT_NULL);  // null terminator
  initialStackFrame.push_back(0);

  size_t stackFrameSize = initialStackFrame.size() * 8;

  uint64_t stackOffset = upAlign(stackFrameSize, 32);
  stackPointer -= stackOffset;

  char* stackFrameBytes = reinterpret_cast<char*>(initialStackFrame.data());
  std::vector<char> data(stackFrameBytes, stackFrameBytes + stackFrameSize);
  paddr = pageTable_->translate(stackPointer);
  os_.getSendToMem()(data, paddr, stackFrameSize);

  stackPointer_ = stackPointer;
  return stackPointer;
}

}  // namespace kernel
}  // namespace simeng
