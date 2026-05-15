#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <tuple>

#include "simeng/Config.hh"
#include "simeng/config/SimInfo.hh"
#include "simeng/Core.hh"
#include "simeng/CoreInstance.hh"
#include "simeng/MemoryInterface.hh"
#include "simeng/OS/SimOS.hh"
#include "simeng/memory/SimpleMem.hh"
#include "simeng/version.hh"

// Global pointers for signal handler to access simulation state.
static simeng::Core* g_core = nullptr;
static std::chrono::high_resolution_clock::time_point g_startTime;
static volatile sig_atomic_t g_interrupted = 0;

static void printStats(int /*sig*/) {
  g_interrupted = 1;
}

static void flushStats() {
  if (!g_core) return;
  auto endTime = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(endTime - g_startTime)
          .count();
  uint64_t retired = g_core->getInstructionsRetiredCount();
  auto stats = g_core->getStats();
  double khz = 0.0, mips = 0.0;
  if (duration > 0) {
    long long cycles = 0;
    auto it = stats.find("cycles");
    if (it != stats.end()) cycles = std::stoll(it->second);
    khz = (cycles / (static_cast<double>(duration) / 1000.0)) / 1000.0;
    mips = (retired / (static_cast<double>(duration))) / 1000.0;
  }
  std::cout << std::endl;
  for (const auto& [key, value] : stats) {
    std::cout << "[SimEng] " << key << ": " << value << std::endl;
  }
  std::cout << std::endl;
  std::cout << "[SimEng] Interrupted after " << (duration / 1000.0)
            << "s (" << std::round(khz) << " kHz, "
            << std::setprecision(2) << mips << " MIPS)" << std::endl;
  std::cout.flush();
}

/** Create a SimOS object depending on whether a binary file was specified. */
simeng::OS::SimOS simOsFactory(std::shared_ptr<simeng::memory::Mem> memory,
                               std::string executablePath,
                               std::vector<std::string> executableArgs) {
  if (executablePath == DEFAULT_STR) {
    simeng::span<char> defaultPrg = simeng::span<char>(
        reinterpret_cast<char*>(simeng::OS::hex_), sizeof(simeng::OS::hex_));
    return simeng::OS::SimOS(memory, defaultPrg);
  }
  return simeng::OS::SimOS(memory, executablePath, executableArgs);
}

/** Tick the provided core model until it halts, a signal is received, or
 *  max_seconds of wall time have elapsed (0 = unlimited). */
int simulate(simeng::OS::SimOS& simOS, simeng::Core& core,
             simeng::MemoryInterface& dataMemory,
             simeng::MemoryInterface& instructionMemory,
             double max_seconds = 0.0) {
  uint64_t iterations = 0;
  // Check time every 100k cycles to keep overhead low.
  const uint64_t check_interval = 100000;
  auto deadline = g_startTime;
  bool limited = (max_seconds > 0.0);
  if (limited)
    deadline = g_startTime +
               std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
                   std::chrono::duration<double>(max_seconds));

  while (!simOS.hasHalted() || dataMemory.hasPendingRequests()) {
    if (g_interrupted) break;
    simOS.tick();
    core.tick();
    instructionMemory.tick();
    dataMemory.tick();
    iterations++;
    if (limited && (iterations % check_interval == 0)) {
      if (std::chrono::high_resolution_clock::now() >= deadline) {
        g_interrupted = 1;
        break;
      }
    }
  }
  return iterations;
}

int main(int argc, char** argv) {
  std::cout << "[SimEng] Build metadata:" << std::endl;
  std::cout << "[SimEng] \tVersion: " SIMENG_VERSION << std::endl;
  std::cout << "[SimEng] \tCompile Time - Date: " __TIME__ " - " __DATE__
            << std::endl;
  std::cout << "[SimEng] \tBuild type: " SIMENG_BUILD_TYPE << std::endl;
  std::cout << "[SimEng] \tCompile options: " SIMENG_COMPILE_OPTIONS
            << std::endl;
  std::cout << "[SimEng] \tTest suite: " SIMENG_ENABLE_TESTS << std::endl;
  std::cout << std::endl;

  std::string executablePath = DEFAULT_STR;
  std::vector<std::string> executableArgs;
  if (argc > 1) {
    std::cout << "[SimEng] Loading config: " << argv[1] << std::endl;
    Config::set(std::string(argv[1]));
    simeng::config::SimInfo::setConfig(std::string(argv[1]));
    std::cout << "[SimEng] Config loaded." << std::endl;
    if (argc > 2) {
      executablePath = std::string(argv[2]);
      char** startOfArgs = argv + 3;
      int numberofArgs = argc - 3;
      executableArgs =
          std::vector<std::string>(startOfArgs, startOfArgs + numberofArgs);
    }
  }

  const size_t memorySize =
      Config::get()["Simulation-Memory"]["Size"].as<size_t>();
  std::shared_ptr<simeng::memory::Mem> memory =
      std::make_shared<simeng::memory::SimpleMem>(memorySize);

  std::cout << "[SimEng] Creating SimOS..." << std::endl;
  simeng::OS::SimOS OS = simOsFactory(memory, executablePath, executableArgs);
  std::cout << "[SimEng] SimOS created." << std::endl;

  VAddrTranslator fn = OS.getVAddrTranslator();
  std::shared_ptr<simeng::memory::MMU> mmu =
      std::make_shared<simeng::memory::MMU>(memory, fn, 0);

  std::unique_ptr<simeng::CoreInstance> coreInstance =
      std::make_unique<simeng::CoreInstance>(memory, mmu,
                                             OS.getSyscallReceiver());

  std::shared_ptr<simeng::Core> core = coreInstance->getCore();
  std::shared_ptr<simeng::MemoryInterface> dataMemory =
      coreInstance->getDataMemory();
  std::shared_ptr<simeng::MemoryInterface> instructionMemory =
      coreInstance->getInstructionMemory();

  OS.registerCore(core);

  std::cout << "[SimEng] Running in " << coreInstance->getSimulationModeString()
            << " mode" << std::endl;
  std::cout << "[SimEng] Workload: " << executablePath;
  for (const auto& arg : executableArgs) std::cout << " " << arg;
  std::cout << std::endl;
  std::cout << "[SimEng] Config file: " << Config::getPath() << std::endl;

  // Install signal handlers so stats are printed on timeout/ctrl-c.
  g_core = core.get();
  std::signal(SIGTERM, printStats);
  std::signal(SIGINT, printStats);

  double max_seconds = 0.0;
  const char* ms_env = std::getenv("SIMENG_MAX_SECONDS");
  if (ms_env) max_seconds = std::atof(ms_env);

  std::cout << "[SimEng] Starting...\n" << std::endl;
  g_startTime = std::chrono::high_resolution_clock::now();
  int iterations = simulate(OS, *core, *dataMemory, *instructionMemory, max_seconds);

  if (g_interrupted) {
    flushStats();
    return 0;
  }

  auto endTime = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(endTime - g_startTime)
          .count();
  double khz = (iterations / (static_cast<double>(duration) / 1000.0)) / 1000.0;
  uint64_t retired = core->getInstructionsRetiredCount();
  double mips = (retired / (static_cast<double>(duration))) / 1000.0;

  std::cout << std::endl;
  auto stats = core->getStats();
  for (const auto& [key, value] : stats) {
    std::cout << "[SimEng] " << key << ": " << value << std::endl;
  }
  std::cout << std::endl;
  std::cout << "[SimEng] Finished " << iterations << " ticks in " << duration
            << "ms (" << std::round(khz) << " kHz, " << std::setprecision(2)
            << mips << " MIPS)" << std::endl;

#ifdef YAML_OUTPUT
  YAML::Emitter out;
  out << YAML::BeginDoc << YAML::BeginMap;
  out << YAML::Key << "build metadata" << YAML::Value;
  out << YAML::BeginSeq;
  out << "Version: " SIMENG_VERSION;
  out << "Compile Time - Date: " __TIME__ " - " __DATE__;
  out << "Build type: " SIMENG_BUILD_TYPE;
  out << "Compile options: " SIMENG_COMPILE_OPTIONS;
  out << "Test suite: " SIMENG_ENABLE_TESTS;
  out << YAML::EndSeq;
  for (const auto& [key, value] : stats) {
    out << YAML::Key << key << YAML::Value << value;
  }
  out << YAML::Key << "duration" << YAML::Value << duration;
  out << YAML::Key << "mips" << YAML::Value << mips;
  out << YAML::Key << "cycles_per_sec" << YAML::Value
      << std::stod(stats["cycles"]) / (duration / 1000.0);
  out << YAML::EndMap << YAML::EndDoc;

  std::cout << "YAML-SEQ\n";
  std::cout << out.c_str() << std::endl;
#endif

  return 0;
}
