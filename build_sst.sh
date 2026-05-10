#!/bin/bash
set -e

# Install additional dependencies for SST
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y libtool autoconf automake python3-dev

INSTALL_DIR=/usr/local/sst
mkdir -p $INSTALL_DIR

# 1. Build SST-Core
echo "--- Building SST-Core ---"
tar -xzf sstcore-12.0.1.tar.gz
cd sstcore-12.0.1
./configure --prefix=$INSTALL_DIR --disable-mpi
make -j$(nproc)
make install
cd ..

# 2. Build SST-Elements
echo "--- Building SST-Elements ---"
tar -xzf sstelements-12.0.1.tar.gz
cd sstelements-12.0.1
./configure --prefix=$INSTALL_DIR --with-sst-core=$INSTALL_DIR --disable-mpi
make -j$(nproc)
make install
cd ..

echo "✅ SST 12.0.1 Build Complete!"
echo "Installed to: $INSTALL_DIR"
