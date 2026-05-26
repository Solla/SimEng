#!/bin/bash
# Verify Dhrystone runs to natural exit AND emits its validation output.
set -u
apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq libyaml-cpp0.6 >/dev/null 2>&1
SE=/workspace/SimEng/build-docker/src/tools/simeng/simeng
CFG=/workspace/SimEng/configs/c1_ultra_v096.yaml
DHRY=/workspace/silicon_binaries/dhrystone_aarch64_fresh
export LD_LIBRARY_PATH=/workspace/SimEng/build-docker/src/lib:/usr/local/lib:/usr/lib/x86_64-linux-gnu
OUT=/workspace/SimEng/probe/_dhry_to_end.log
: > "$OUT"
# No SIMENG_MAX_SECONDS. Bounded by external timeout.
# Try a SMALL -l first (1000) so we see if it terminates at all.
echo "===== Dhry -l 1 (1M iters, real) =====" | tee -a "$OUT"
timeout 600 "$SE" "$CFG" "$DHRY" -l 1 >> "$OUT" 2>&1 || true
echo "--- exit signature ---"
tail -5 "$OUT"
echo "--- validation greps ---"
grep -iE "microseconds|dhrystones per second|interrupted|finished|vax mips|loops" "$OUT" | head -20
