import sst
import os

# C1-Ultra SST cache hierarchy config
#
# C1-Ultra is an Arm-licensed IP core (NOT an Apple Silicon CPU).
#
# Purpose: SimEng standalone has no cache-miss model (L1I Flat / L1D Fixed
# never miss), pinning Dhrystone IPC artificially high (4.0 vs ~3.05
# silicon). Under SST, memHierarchy.Cache models real L1/L2 misses so the
# string/struct copies finally pay miss latency.
#
# This rewrite uses the explicit cpulink/memlink MemLink subcomponent
# pattern (per the in-repo working test sst/test/sstconfigs/
# fastL1WithParams_config.py). The previous version used the legacy
# high_network_0 / low_network_0 / direct_link port names, which do not
# bind correctly with the SST memHierarchy in this image — that was the
# failing point.
#
# Topology: SimEng's SST wrapper exposes a single unified memory port, so
# this models a unified L1 (sized as the 128KiB L1D) + 2MiB L2 + DRAM.
# C1-Ultra TRM geometry: L1D 128KiB / L1I 64KiB 4-way 64B 4cy; L2 2MiB
# 8-way 64B ~10cy.

SIMENG_CONFIG = os.environ.get(
    "SIMENG_CONFIG", "/workspace/SimEng/configs/c1_ultra_v096.yaml")
EXECUTABLE = os.environ.get("SIMENG_EXE_PATH", "/workspace/dhrystone_aarch64")
CLOCK_FREQ = "3.5GHz"
CACHE_FREQ = "3.5Ghz"
MEM_SIZE = "2GiB"
CLW = "64"  # cache line width (bytes); must match cache_line_size

DEBUG = 0
DEBUG_LEVEL = 0

# SimEng core
cpu = sst.Component("core", "sstsimeng.simengcore")
cpu.addParams({
    "simeng_config_path": SIMENG_CONFIG,
    "executable_path": EXECUTABLE,
    "executable_args": "",
    "clock": CLOCK_FREQ,
    "max_addr_memory": 2 * 1024 * 1024 * 1024 - 1,
    "cache_line_width": CLW,
    "source": "",
    "assemble_with_source": False,
    "heap": "",
    "debug": False
})
iface = cpu.setSubComponent("memory", "memHierarchy.standardInterface")

# L1 (unified; 128KiB L1D geometry, 4-way, 4-cycle hit)
l1cache = sst.Component("l1cache.mesi", "memHierarchy.Cache")
l1cache.addParams({
    "access_latency_cycles": "4",
    "cache_frequency": CACHE_FREQ,
    "replacement_policy": "lru",
    "coherence_protocol": "MESI",
    "associativity": "4",
    "cache_line_size": CLW,
    "cache_size": "128KiB",
    "L1": "1",
    "debug": DEBUG,
    "debug_level": DEBUG_LEVEL,
    "verbose": "1"
})
l1toC = l1cache.setSubComponent("cpulink", "memHierarchy.MemLink")
l1toL2 = l1cache.setSubComponent("memlink", "memHierarchy.MemLink")

# L2 (2MiB, 8-way, ~10-cycle hit)
l2cache = sst.Component("l2cache.mesi", "memHierarchy.Cache")
l2cache.addParams({
    "access_latency_cycles": "10",
    "cache_frequency": CACHE_FREQ,
    "replacement_policy": "lru",
    "coherence_protocol": "MESI",
    "associativity": "8",
    "cache_line_size": CLW,
    "cache_size": "2MiB",
    "L1": "0",
    "debug": DEBUG,
    "debug_level": DEBUG_LEVEL,
    "verbose": "1"
})
l2toL1 = l2cache.setSubComponent("cpulink", "memHierarchy.MemLink")
l2toM = l2cache.setSubComponent("memlink", "memHierarchy.MemLink")

# DRAM controller (~100ns backing)
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "request_width": "64",
    "debug": DEBUG,
    "debug_level": DEBUG_LEVEL,
    "addr_range_end": 2 * 1024 * 1024 * 1024 - 1,
})
Mtol2 = memctrl.setSubComponent("cpulink", "memHierarchy.MemLink")
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time": "100 ns",
    "mem_size": MEM_SIZE,
    "request_width": "64"
})

# Links: CPU <-> L1 <-> L2 <-> MemCtrl
link_cpu_l1 = sst.Link("link_cpu_l1")
link_cpu_l1.connect((iface, "port", "0ps"), (l1toC, "port", "0ps"))
link_l1_l2 = sst.Link("link_l1_l2")
link_l1_l2.connect((l1toL2, "port", "100ps"), (l2toL1, "port", "100ps"))
link_l2_mem = sst.Link("link_l2_mem")
link_l2_mem.connect((l2toM, "port", "100ps"), (Mtol2, "port", "100ps"))

# Cache statistics
sst.setStatisticLoadLevel(5)
sst.setStatisticOutput("sst.statOutputConsole")
l1cache.enableStatistics(["CacheHits", "CacheMisses", "TotalEventsReceived"])
l2cache.enableStatistics(["CacheHits", "CacheMisses", "TotalEventsReceived"])
