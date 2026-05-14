#!/bin/bash
# Run hello_simeng

apt-get update -qq && apt-get install -y libyaml-cpp-dev > /dev/null 2>&1

GEEKBENCH_BIN=/workspace/SimEng/hello_simeng
CONFIG=/workspace/SimEng/configs/emulation.yaml
SIMENG_EXE=/workspace/SimEng/build-docker/src/tools/simeng/simeng

export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/usr/local/sst/lib:$LD_LIBRARY_PATH

echo "Starting hello_simeng simulation..."
$SIMENG_EXE $CONFIG $GEEKBENCH_BIN > /workspace/SimEng/test_hello.log 2>&1
echo "Finished. Check /workspace/SimEng/test_hello.log"
