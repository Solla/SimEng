import sst
import os

# --- Configuration ---
# Set paths for SimEng
SIMENG_CONFIG = "configs/sst-cores/c1_ultra-sst.yaml"  # Path relative to SimEng root
EXECUTABLE = "SimEngDefaultProgram"         # Path relative to SimEng root
CLOCK_FREQ = "3.5GHz"
MEM_SIZE = "2GiB"
CACHE_LINE_WIDTH = "64"

# --- Components ---

# 1. SimEng Core
cpu = sst.Component("core", "sstsimeng.simengcore")
cpu.addParams({
    "simeng_config_path": SIMENG_CONFIG,
    "executable_path": EXECUTABLE,
    "executable_args": "",
    "clock" : CLOCK_FREQ,
    "max_addr_memory": 2*1024*1024*1024-1,
    "cache_line_width": CACHE_LINE_WIDTH,
    "debug": False
})

# StandardInterface to connect CPU to memory hierarchy
iface = cpu.setSubComponent("memory", "memHierarchy.standardInterface")

# 2. L1 Cache (128KB, 4-cycle)
l1cache = sst.Component("l1cache", "memHierarchy.Cache")
l1cache.addParams({
    "access_latency_cycles" : "4",
    "cache_frequency" : CLOCK_FREQ,
    "replacement_policy" : "lru",
    "coherence_protocol" : "MESI",
    "associativity" : "8",
    "cache_line_size" : CACHE_LINE_WIDTH,
    "cache_size" : "128KiB",
    "L1" : "1",
    "verbose": "0"
})

# 3. L2 Cache (2MB, 10-cycle)
l2cache = sst.Component("l2cache", "memHierarchy.Cache")
l2cache.addParams({
    "access_latency_cycles" : "10",
    "cache_frequency" : CLOCK_FREQ,
    "replacement_policy" : "lru",
    "coherence_protocol" : "MESI",
    "associativity" : "8",
    "cache_line_size" : CACHE_LINE_WIDTH,
    "cache_size" : "2MiB",
    "verbose": "0"
})

# 4. L3 Cache (16MB, 60-cycle)
l3cache = sst.Component("l3cache", "memHierarchy.Cache")
l3cache.addParams({
    "access_latency_cycles" : "60",
    "cache_frequency" : CLOCK_FREQ,
    "replacement_policy" : "lru",
    "coherence_protocol" : "MESI",
    "associativity" : "16",
    "cache_line_size" : CACHE_LINE_WIDTH,
    "cache_size" : "16MiB",
    "verbose": "0"
})

# 5. Memory Controller
memctrl = sst.Component("memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock" : "1GHz",
    "addr_range_end" : 2*1024*1024*1024-1,
})

# 6. Memory Backend (DDR 100ns)
memory = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
memory.addParams({
    "access_time" : "100ns",
    "mem_size" : MEM_SIZE,
})

# --- Links ---

# CPU <-> L1
link_cpu_l1 = sst.Link("link_cpu_l1")
link_cpu_l1.connect((iface, "port", "10ps"), (l1cache, "high_network_0", "10ps"))

# L1 <-> L2
link_l1_l2 = sst.Link("link_l1_l2")
link_l1_l2.connect((l1cache, "low_network_0", "50ps"), (l2cache, "high_network_0", "50ps"))

# L2 <-> L3
link_l2_l3 = sst.Link("link_l2_l3")
link_l2_l3.connect((l2cache, "low_network_0", "50ps"), (l3cache, "high_network_0", "50ps"))

# L3 <-> Memory
link_l3_mem = sst.Link("link_l3_mem")
link_l3_mem.connect((l3cache, "low_network_0", "100ps"), (memctrl, "direct_link", "100ps"))

# --- Statistics ---
# Enable statistics for all cache components
sst.setStatisticLoadLevel(5)
sst.setStatisticOutput("sst.statOutputConsole")

# Enable stats for specific cache components
l1cache.enableStatistics(["CacheHits", "CacheMisses", "TotalEventsReceived"])
l2cache.enableStatistics(["CacheHits", "CacheMisses", "TotalEventsReceived"])
l3cache.enableStatistics(["CacheHits", "CacheMisses", "TotalEventsReceived"])
