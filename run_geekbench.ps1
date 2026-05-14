# Windows script to run Geekbench 6 in SimEng via Docker

param (
    [switch]$sst
)

$SimEngRoot = Get-Location
$ParentDir = Split-Path $SimEngRoot -Parent

$args = ""
if ($sst) {
    $args = "--sst"
}

docker run --rm `
    -v "${SimEngRoot}:/workspace/SimEng" `
    -v "${ParentDir}/Geekbench-6.7.1-LinuxARMPreview:/workspace/Geekbench-6.7.1-LinuxARMPreview" `
    simeng-dev `
    bash /workspace/SimEng/run_geekbench_docker.sh $args
