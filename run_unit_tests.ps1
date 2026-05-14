param (
    [int]$Jobs = 0
)

$SimEngRoot = "C:\Users\Solla\Documents\apple_m1_SimEng"

$jobsArg = if ($Jobs -gt 0) { $Jobs } else { '$(nproc)' }

docker run --rm `
    -v "${SimEngRoot}:/workspace" `
    simeng-dev `
    bash -c "apt-get update -qq && apt-get install -y wget libyaml-cpp-dev 2>&1 | tail -3; \
             wget -qO- https://github.com/Kitware/CMake/releases/download/v3.26.4/cmake-3.26.4-linux-x86_64.tar.gz | tar --strip-components=1 -xz -C /usr/local && \
             cmake --version && \
             mkdir -p /workspace/SimEng/build-tests-aarch64 && cd /workspace/SimEng/build-tests-aarch64 && cmake /workspace/SimEng -DCMAKE_BUILD_TYPE=Release -DSIMENG_ENABLE_TESTS=ON -DSIMENG_ARCH=AArch64 2>&1 && make unittests -j$jobsArg 2>&1 && ./test/unit/unittests"
