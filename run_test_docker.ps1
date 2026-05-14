$SimEngRoot = "C:\Users\Solla\Documents\apple_m1_SimEng"

docker run --rm `
    -v "${SimEngRoot}:/workspace" `
    simeng-dev `
    bash -c "cd /workspace/SimEng && bash run_test_prg.sh"
