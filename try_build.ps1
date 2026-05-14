param (
    [int]$Jobs = 0
)

$SimEngRoot = "C:\Users\Solla\Documents\apple_m1_SimEng"

$jobsArg = if ($Jobs -gt 0) { $Jobs } else { '$(nproc)' }

docker run --rm `
    -v "${SimEngRoot}:/workspace" `
    simeng-dev `
    bash -c "apt-get update -qq && apt-get install -y libyaml-cpp-dev 2>&1 | tail -5 && cd /workspace/SimEng && mkdir -p build-docker && cd build-docker && cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 && make -j$jobsArg 2>&1"
