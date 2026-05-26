#!/bin/bash
# Run CM standalone with iterations=1 (minimum) to actual termination.
set -u
apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq libyaml-cpp0.6 >/dev/null 2>&1
SE=/workspace/SimEng/build-docker/src/tools/simeng/simeng
CFG=/workspace/SimEng/configs/c1_ultra_v096.yaml
CM=/workspace/silicon_binaries/coremark_aarch64
export LD_LIBRARY_PATH=/workspace/SimEng/build-docker/src/lib:/usr/local/lib:/usr/lib/x86_64-linux-gnu
OUT=/workspace/SimEng/probe/_cm_to_end.log
: > "$OUT"
# argv: seed1 seed2 seed3 iterations [execs]  (performance run: 0 0 0x66)
echo "===== CM iter=100 perf-run =====" | tee -a "$OUT"
export SIMENG_TAGE_PROFILE=1
timeout 1500 "$SE" "$CFG" "$CM" 0 0 0x66 100 >> "$OUT" 2>&1 || true
echo "--- validation greps ---"
grep -iE "seedcrc|crclist|CoreMark 1\.0|Iterations|Interrupted|Unrecognised|exit_group" "$OUT" | head -20
