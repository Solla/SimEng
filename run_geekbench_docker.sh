#!/bin/bash
set -ex

# Exit on error
set -e

# 1. Install dependencies if not present
if ! dpkg -s libc6-arm64-cross libyaml-cpp-dev >/dev/null 2>&1; then
    echo "Installing dependencies..."
    apt-get update && apt-get install -y libc6-arm64-cross libyaml-cpp-dev
else
    echo "Dependencies already installed."
fi

# 2. Setup special-files directory for other libraries if needed
mkdir -p /workspace/SimEng/special-files/lib
mkdir -p /workspace/SimEng/special-files/usr/lib

# Symlink the entire aarch64-linux-gnu lib folder
if [ ! -e /workspace/SimEng/special-files/lib/aarch64-linux-gnu ]; then
    ln -s /usr/aarch64-linux-gnu/lib /workspace/SimEng/special-files/lib/aarch64-linux-gnu
fi
if [ ! -e /workspace/SimEng/special-files/usr/lib/aarch64-linux-gnu ]; then
    ln -s /usr/aarch64-linux-gnu/lib /workspace/SimEng/special-files/usr/lib/aarch64-linux-gnu
fi
# Also symlink the main lib directly if needed
if [ ! -e /workspace/SimEng/special-files/lib/ld-linux-aarch64.so.1 ]; then
    ln -s /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 /workspace/SimEng/special-files/lib/ld-linux-aarch64.so.1
fi

# 3. Run Geekbench 6 in SimEng
GEEKBENCH_BIN="/workspace/Geekbench-6.7.1-LinuxARMPreview/geekbench_aarch64"
CONFIG="/workspace/SimEng/configs/c1_ultra_geekbench.yaml"
SIMENG_EXE="/workspace/SimEng/build-docker/src/tools/simeng/simeng"

# Setup sysroot symlinks for multiarch
mkdir -p /usr/aarch64-linux-gnu/usr
cp -rL /usr/aarch64-linux-gnu/lib /usr/aarch64-linux-gnu/usr/lib
(cd /usr/aarch64-linux-gnu/lib && ln -sf . aarch64-linux-gnu)
ls -l /usr/aarch64-linux-gnu/usr/lib/libdl.so.2
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
echo "Starting Geekbench 6 simulation..."
if [ "$1" == "--sst" ]; then
    echo "Running with SST (16M L3 Cache)..."
    export SIMENG_CONFIG=$CONFIG
    export SIMENG_EXE_PATH=$GEEKBENCH_BIN
    # Use --lib-path to ensure SST finds the SimEng library
    sst --lib-path=/workspace/SimEng/build-docker/sst /workspace/SimEng/sst_cache_config.py
else
    echo "Running standalone SimEng..."
    $SIMENG_EXE $CONFIG $GEEKBENCH_BIN > /workspace/SimEng/simeng.log 2>&1
    cat /workspace/SimEng/simeng.log
fi
