// clang-format off
// DO NOT MOVE FROM TOP OF FILE - https://github.com/sstsimulator/sst-core/issues/865
#include <sst/core/sst_config.h>
// clang-format on

#include "SimEngCoreWrapper.hh"

#include <cstdlib>
#include <iostream>

#include "Assemble.hh"

using namespace SST::SSTSimEng;
using namespace SST::Interfaces;

SimEngCoreWrapper::SimEngCoreWrapper(SST::ComponentId_t id, SST::Params& params)
    : SST::Component(id) {
  output_.init("[SSTSimEng:SimEngCoreWrapper] " + getName() + ":@p:@l ", 999, 0,
               SST::Output::STDOUT);
  clock_ = registerClock(params.find<std::string>("clock", "1GHz"),
                         new SST::Clock::Handler<SimEngCoreWrapper>(
                             this, &SimEngCoreWrapper::clockTick));

  // Extract variables from config.py
  executablePath_ = params.find<std::string>("executable_path", "");
  executableArgs_ = splitArgs(params.find<std::string>("executable_args", ""));
  simengConfigPath_ = params.find<std::string>("simeng_config_path", "");
  cacheLineWidth_ = params.find<uint64_t>("cache_line_width", "64");
  maxAddrMemory_ = params.find<uint64_t>("max_addr_memory", "0");
  source_ = params.find<std::string>("source", "");
  assembleWithSource_ = params.find<bool>("assemble_with_source", false);
  heapStr_ = params.find<std::string>("heap", "");
  debug_ = params.find<bool>("debug", false);

  // Optional wall-clock cap, mirroring src/tools/simeng/main.cc's
  // SIMENG_MAX_SECONDS. Many SimEng benchmark binaries (Dhrystone here) loop
  // on a very large run count and are measured at a time cap rather than at
  // program completion; without this the SST simulation never prints stats.
  // A "max_seconds" component param takes precedence over the env var.
  maxSeconds_ = params.find<double>("max_seconds", 0.0);
  if (maxSeconds_ <= 0.0) {
    const char* ms = std::getenv("SIMENG_MAX_SECONDS");
    if (ms) maxSeconds_ = std::atof(ms);
  }

  if (executablePath_.length() == 0 && !assembleWithSource_) {
    output_.verbose(CALL_INFO, 10, 0,
                    "SimEng executable binary filepath not provided.");
    std::exit(EXIT_FAILURE);
  }
  if (maxAddrMemory_ == 0) {
    output_.verbose(CALL_INFO, 10, 0,
                    "Maximum address range for memory not provided");
    std::exit(EXIT_FAILURE);
  }

  iterations_ = 0;

  // Instantiate the StandardMem Interface defined in config.py. The
  // SimEngMemInterface and its response handler are constructed later in
  // fabricateSimEngCore(), once the MMU exists (it now needs the MMU to
  // service accesses functionally). This is safe because StandardMem
  // responses only arrive during clockTick(), after init().
  sstMem_ = loadUserSubComponent<SST::Interfaces::StandardMem>(
      "memory", ComponentInfo::SHARE_NONE, clock_,
      new StandardMem::Handler<SimEngCoreWrapper>(
          this, &SimEngCoreWrapper::handleMemoryEvent));

  // Protected methods from SST::Component used to start simulation
  registerAsPrimaryComponent();
  primaryComponentDoNotEndSim();
}

SimEngCoreWrapper::~SimEngCoreWrapper() {}

void SimEngCoreWrapper::setup() {
  sstMem_->setup();
  output_.verbose(CALL_INFO, 1, 0, "Memory setup complete\n");
  // Run Simulation
  std::cout << "[SimEng] Starting...\n" << std::endl;
  startTime_ = std::chrono::high_resolution_clock::now();
}

void SimEngCoreWrapper::handleMemoryEvent(StandardMem::Request* memEvent) {
  memEvent->handle(handlers_);
}

void SimEngCoreWrapper::finish() {
  output_.verbose(CALL_INFO, 1, 0,
                  "Simulation complete. Finalising stats....\n");

  auto endTime = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime_)
                      .count();
  double khz =
      (iterations_ / (static_cast<double>(duration) / 1000.0)) / 1000.0;
  uint64_t retired = core_->getInstructionsRetiredCount();
  double mips = (retired / (static_cast<double>(duration))) / 1000.0;

  // Print stats
  std::cout << "\n";
  auto stats = core_->getStats();
  for (const auto& [key, value] : stats) {
    std::cout << "[SimEng] " << key << ": " << value << "\n";
  }

  std::cout << "\n[SimEng] Finished " << iterations_ << " ticks in " << duration
            << "ms (" << std::round(khz) << " kHz, " << std::setprecision(2)
            << mips << " MIPS)" << std::endl;
}

void SimEngCoreWrapper::init(unsigned int phase) {
  sstMem_->init(phase);
  // Init can have multiple phases, only fabricate the core once at phase 0
  if (phase == 0) {
    fabricateSimEngCore();
  }
}

bool SimEngCoreWrapper::clockTick(SST::Cycle_t current_cycle) {
  // Tick SimOS, the core and the memory interfaces until the program has
  // halted, mirroring the simulate() loop in src/tools/simeng/main.cc.
  if (!os_->hasHalted() || dataMemory_->hasPendingRequests()) {
    // Tick the OS kernel (scheduling, syscalls, page faults).
    os_->tick();

    // Tick the core.
    core_->tick();

    // Tick the instruction memory.
    instructionMemory_->tick();

    // Tick the data memory.
    dataMemory_->tick();

    iterations_++;

    // Periodic heartbeat + wall-clock cap check (cheap: every 100k cycles).
    if ((iterations_ % 100000) == 0) {
      if (debug_ && (iterations_ % 2000000) == 0) {
        std::cerr << "[SSTSimEng] heartbeat: cycle " << iterations_
                  << " retired " << core_->getInstructionsRetiredCount()
                  << " dMemPending " << dataMemory_->hasPendingRequests()
                  << std::endl;
      }
      if (maxSeconds_ > 0.0) {
        auto now = std::chrono::high_resolution_clock::now();
        double elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                  startTime_)
                .count() /
            1000.0;
        if (elapsed >= maxSeconds_) {
          std::cout << "\n[SimEng] Reached SIMENG_MAX_SECONDS (" << maxSeconds_
                    << "s) at cycle " << iterations_ << " — ending simulation."
                    << std::endl;
          // End the SST simulation cleanly; SST then calls finish() (SimEng
          // stats) and emits the enabled L1/L2 cache statistics.
          primaryComponentOKToEndSim();
          return true;
        }
      }
    }

    return false;
  } else {
    // Protected method from SST::Component used to end SST simulation
    primaryComponentOKToEndSim();
    return true;
  }
}
std::string SimEngCoreWrapper::trimSpaces(std::string strArgs) {
  int trailingEnd = -1;
  int leadingEnd = -1;
  for (int x = 0; x < strArgs.size(); x++) {
    int end = strArgs.size() - 1 - x;
    // Find the index, from the start of the string, which is not a space.
    if (strArgs.at(x) != ' ' && leadingEnd == -1) {
      leadingEnd = x;
    }
    // Find the index, from the end of the string, which is not a space.
    if (strArgs.at(end) != ' ' && trailingEnd == -1) {
      trailingEnd = end;
    }
    if (trailingEnd != -1 && leadingEnd != -1) {
      break;
    }
  }
  // The string has leading or trailing spaces, return the substring which
  // doesn't have those spaces.
  if (trailingEnd != -1 && leadingEnd != -1) {
    return strArgs.substr(leadingEnd, trailingEnd - leadingEnd + 1);
  }
  // The string does not have leading or trailing spaces, return the original
  // string.
  return strArgs;
};

std::vector<std::string> SimEngCoreWrapper::splitArgs(std::string strArgs) {
  std::string trimmedStrArgs = trimSpaces(strArgs);
  std::string str = "";
  std::vector<std::string> args;
  std::size_t argSize = trimmedStrArgs.size();
  bool escapeSingle = false;
  bool escapeDouble = false;
  bool captureEscape = false;
  uint64_t index = 0;
  if (argSize == 0) {
    return args;
  }

  for (int x = 0; x < argSize; x++) {
    index = x;
    bool escaped = escapeDouble || escapeSingle;
    char currChar = trimmedStrArgs.at(x);
    if (captureEscape) {
      captureEscape = false;
      str += currChar;
    }
    // This if statement check for an escaped '\' in the string.
    // Any character after the '\' is appended to the current argument,
    // without any delimiting or escape behaviour.
    else if (currChar == '\\') {
      captureEscape = true;
    } else if (escaped) {
      // If a portion of the argument string starts with a single quote (") and
      // we encounter another single quote, capture the substring enclosed by a
      // valid set of single quotes into an argument without producing any
      // delimiting or escape behavior even with double quotes.
      // e.g "arg1=1 arg2='"Hi"' arg3=2" will be parsed as
      // std::vector<std::string>{arg1=1, arg2="Hi", arg3=2}
      if (currChar == '\'' && escapeSingle) {
        escapeSingle = 0;
      }
      // If a portion of the argument string starts with a double quote (") and
      // we encounter another double quote, capture the substring enclosed by a
      // valid set of double quotes into an argument without producing any
      // delimiting or escape behavior even with single quotes.
      // e.g "arg1=1 arg2="James' Car" arg3=2" will be parsed as
      // std::vector<std::string>{arg1=1, arg2=James' Car, arg3=2}
      else if (currChar == '\"' && escapeDouble) {
        escapeDouble = 0;
      } else {
        str += currChar;
      }
    } else {
      if (currChar == ' ') {
        if (str != "") {
          args.push_back(str);
          str = "";
        }
      }
      // Check for escape character ("), this signals the algorithm to capture
      // any char inside a set of ("") without producing any delimiting or
      // escape behavior.
      else if (currChar == '\"') {
        escapeDouble = 1;
        // Check for escape character ('), this signals the algorithm to capture
        // any char inside a set of ('') without producing any delimiting or
        // escape behavior.
      } else if (currChar == '\'') {
        escapeSingle = 1;
      } else {
        str += currChar;
      }
    }
  }
  if (escapeSingle || escapeDouble) {
    std::string err;
    output_.verbose(CALL_INFO, 1, 0, R"(
           Parsing failed: Invalid format - Please make sure all
           characters/strings are escaped properly within a set single or 
           double quotes. To escape quotes use (\\\) instead of (\).\n
           )");
    std::cerr << "[SSTSimEng:SimEngCoreWrapper] Error occured at index "
              << index << " of the argument string - substring: "
              << "[ " << str << " ]" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  args.push_back(str);
  return args;
}

void SimEngCoreWrapper::initialiseHeapData() {
  std::vector<char> initialHeapData;
  std::vector<uint64_t> heapVals = splitHeapStr();
  uint64_t heapSize = heapVals.size() * 8;
  initialHeapData.resize(heapSize);
  uint64_t* heap = reinterpret_cast<uint64_t*>(initialHeapData.data());
  for (size_t x = 0; x < heapVals.size(); x++) {
    heap[x] = heapVals[x];
  }
  // Post-SimOS the process image lives in `memory_` and is addressed
  // virtually. Translate the heap start through the OS page table and write
  // the test data untimed, directly into simulation memory.
  uint64_t heapStart = os_->getProcess(1)->getHeapStart();
  uint64_t paddr = os_->handleVAddrTranslation(heapStart, 1);
  if (simeng::OS::masks::faults::getFaultCode(paddr) ==
      simeng::OS::masks::faults::pagetable::NO_FAULT) {
    memory_->sendUntimedData(initialHeapData, paddr, heapSize);
  }
}

void SimEngCoreWrapper::fabricateSimEngCore() {
  output_.verbose(CALL_INFO, 1, 0, "Setting up SimEng Core\n");
  uint8_t* assembled_source = NULL;
  size_t assembled_source_size = 0;
  if (assembleWithSource_) {
    output_.verbose(CALL_INFO, 1, 0,
                    "Assembling source instructions using LLVM\n");
    Assembler assemble = Assembler(source_);
    assembled_source = assemble.getAssembledSource();
    assembled_source_size = assemble.getAssembledSourceSize();
  }
  // Select and load the model configuration. Post-SimOS, both the legacy
  // yaml-cpp `Config` (read by CoreInstance) and the newer ryml `SimInfo`
  // must be populated, exactly as src/tools/simeng/main.cc does.
  std::string configPath = simengConfigPath_;
  if (configPath == "") {
    output_.verbose(CALL_INFO, 1, 0,
                    "No SimEng configuration provided. Using the default "
                    "a64fx-sst.yaml configuration file.\n");
    configPath = a64fxConfigPath_;
  }
  Config::set(configPath);
  simeng::config::SimInfo::setConfig(configPath);

  // SST integration requires the L1 data interface to be External so that
  // the SST-backed SimEngMemInterface can be injected. Force it here rather
  // than depending on the YAML, so the tuned standalone config
  // (c1_ultra_v096.yaml) can be reused unchanged under SST.
  Config::get()["L1-Data-Memory"]["Interface-Type"] = "External";

  if (config::SimInfo::getSimMode() != config::SimulationMode::Outoforder) {
    output_.verbose(CALL_INFO, 1, 0,
                    "SimEng currently only supports Out-of-Order "
                    "archetypes with SST.");
    std::exit(EXIT_FAILURE);
  }

  // Build the SimOS stack exactly as main.cc's simulate path does:
  //   SimpleMem -> SimOS -> MMU -> CoreInstance.
  // SST does not replace SimOS; it replaces only the *timing* of the L1
  // data path. SimpleMem remains the functional source of truth (process
  // image, page tables, syscall buffers, instruction fetch).
  const size_t memorySize =
      Config::get()["Simulation-Memory"]["Size"].as<size_t>();
  memory_ = std::make_shared<simeng::memory::SimpleMem>(memorySize);

  if (assembleWithSource_) {
    simeng::span<char> instrBytes(reinterpret_cast<char*>(assembled_source),
                                  assembled_source_size);
    os_ = std::make_unique<simeng::OS::SimOS>(memory_, instrBytes);
  } else {
    os_ = std::make_unique<simeng::OS::SimOS>(memory_, executablePath_,
                                              executableArgs_);
  }

  VAddrTranslator translator = os_->getVAddrTranslator();
  mmu_ = std::make_shared<simeng::memory::MMU>(memory_, translator, 0);

  // The SST-backed L1 data interface: functional access via the MMU,
  // timing via the SST cache hierarchy.
  dataMemory_ = std::make_shared<SimEngMemInterface>(
      sstMem_, mmu_, cacheLineWidth_, maxAddrMemory_, debug_);
  handlers_ = new SimEngMemInterface::SimEngMemHandlers(*dataMemory_, &output_);

  coreInstance_ = std::make_unique<simeng::CoreInstance>(
      memory_, mmu_, os_->getSyscallReceiver());

  // Set the SST-backed data memory and construct the core (L1-Data-Memory
  // is External, so CoreInstance deferred core creation to here).
  coreInstance_->setL1DataMemory(dataMemory_);
  coreInstance_->createCore();

  // Get remaining simulation objects needed to forward simulation
  core_ = coreInstance_->getCore();
  instructionMemory_ = coreInstance_->getInstructionMemory();
  os_->registerCore(core_);

  // Ensure the SST backend can hold the whole simulation memory.
  if (maxAddrMemory_ < memorySize) {
    output_.verbose(
        CALL_INFO, 1, 0,
        "Error: SST backend memory is smaller than the SimEng simulation "
        "memory. Increase the memory allocated to memHierarchy.memBackend "
        "and keep it consistent with \'max_addr_memory\' / "
        "\'addr_range_end\'.\n");
    primaryComponentOKToEndSim();
    std::exit(EXIT_FAILURE);
  }
// If testing is enabled populate heap if heap values have been specified.
#ifdef SIMENG_ENABLE_SST_TESTS
  if (heapStr_ != "") {
    initialiseHeapData();
  }
#endif

  output_.verbose(CALL_INFO, 1, 0, "SimEng core setup successfully.\n");
  // Print out build metadata
  std::cout << "[SimEng] Build metadata:" << std::endl;
  std::cout << "[SimEng] \tVersion: " SIMENG_VERSION << std::endl;
  std::cout << "[SimEng] \tCompile Time - Date: " __TIME__ " - " __DATE__
            << std::endl;
  std::cout << "[SimEng] \tBuild type: " SIMENG_BUILD_TYPE << std::endl;
  std::cout << "[SimEng] \tCompile options: " SIMENG_COMPILE_OPTIONS
            << std::endl;
  std::cout << "[SimEng] \tTest suite: " SIMENG_ENABLE_TESTS << std::endl;
  std::cout << std::endl;

  // Output general simulation details
  std::cout << "[SimEng] Running in "
            << simeng::config::SimInfo::getSimModeStr() << " mode" << std::endl;
  std::cout << "[SimEng] Workload: " << executablePath_;
  for (const auto& arg : executableArgs_) std::cout << " " << arg;
  std::cout << std::endl;
  std::cout << "[SimEng] Config file: "
            << simeng::config::SimInfo::getConfigPath() << std::endl;
  std::cout << "[SimEng] ISA: " << simeng::config::SimInfo::getISAString()
            << std::endl;
  std::cout << "[SimEng] Auto-generated Special File directory: ";
  if (simeng::config::SimInfo::getGenSpecFiles())
    std::cout << "True";
  else
    std::cout << "False";
  std::cout << std::endl;
  std::cout << "[SimEng] Special File directory used: "
            << simeng::config::SimInfo::getConfig()["CPU-Info"]
                                                   ["Special-File-Dir-Path"]
                                                       .as<std::string>()
            << std::endl;
  std::cout << "[SimEng] Number of Cores: "
            << simeng::config::SimInfo::getConfig()["CPU-Info"]["Core-Count"]
                   .as<uint16_t>()
            << std::endl;
}

std::vector<uint64_t> SimEngCoreWrapper::splitHeapStr() {
  std::vector<uint64_t> out;
  std::string acc = "";
  for (size_t a = 0; a < heapStr_.size(); a++) {
    if (heapStr_[a] == ',') {
      out.push_back(static_cast<uint64_t>(std::stoull(acc)));
      acc = "";
    } else {
      acc += heapStr_[a];
    }
  }
  out.push_back(static_cast<uint64_t>(std::stoull(acc)));
  return out;
}