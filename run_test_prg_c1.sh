#!/bin/bash
# Run SimEngDefaultProgram with C1 Ultra config

apt-get update -qq && apt-get install -y libyaml-cpp-dev > /dev/null 2>&1

GEEKBENCH_BIN=/workspace/SimEng/SimEngDefaultProgram
CONFIG=/workspace/SimEng/configs/c1_ultra.yaml
SIMENG_EXE=/workspace/SimEng/build-docker/src/tools/simeng/simeng

export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/usr/local/sst/lib:$LD_LIBRARY_PATH

echo "Starting SimEngDefaultProgram simulation with C1 Ultra..."
$SIMENG_EXE $CONFIG $GEEKBENCH_BIN > /workspace/SimEng/test_default_prg_c1.log 2>&1
echo "Finished. Check /workspace/SimEng/test_default_prg_c1.log"
