#!/usr/bin/env bash
# M2.5 variance study driver. Every run records the power state and load average before
# and after, because both change what the machine is capable of.
set -u
cd "$(dirname "$0")/../../.."
BIN=engine/build/release/bench
D=docs/results/m2.5
PS=$D/powerstate.sh
LOG=$D/run_log.txt

run() {   # run <name> <args...>
  local name=$1; shift
  echo "=== $name" | tee -a "$LOG"
  echo "    cmd:    $BIN run $* --out $D/$name.json" | tee -a "$LOG"
  echo "    before: $($PS)  $(uptime | sed 's/.*load average/load/')" | tee -a "$LOG"
  local t0=$SECONDS
  $BIN run "$@" --out "$D/$name.json" 2>>"$LOG"
  local rc=$?
  echo "    after:  $($PS)  $(uptime | sed 's/.*load average/load/')" | tee -a "$LOG"
  echo "    rc=$rc elapsed=$((SECONDS-t0))s" | tee -a "$LOG"
}

: > "$LOG"
echo "M2.5 variance study, started $(date -Is)" | tee -a "$LOG"
echo "engine: $($BIN --version)" | tee -a "$LOG"
echo | tee -a "$LOG"

# ---- Target #2: 1000 trials, the configuration PLAN.md specifies --------------------
# Best thread counts come from M2.4 (Best performance, unpinned): int 16, fp 22, hash 16.
COMMON="-n 1000 --trial-ms 50 --warmup 20 --cold clflush --pin"
run target2_cpu_int  -w cpu_int  -t 1,16 $COMMON
run target2_cpu_fp   -w cpu_fp   -t 1,22 $COMMON
run target2_cpu_hash -w cpu_hash -t 1,16 $COMMON

# ---- knob: --pin on/off (1 thread, 200 trials) --------------------------------------
KNOB="-w cpu_int,cpu_fp,cpu_hash -t 1 -n 200 --trial-ms 50 --warmup 20 --cold clflush"
run knob_pin_on   $KNOB --pin
run knob_pin_off  $KNOB

# ---- knob: warmup on/off ------------------------------------------------------------
run knob_warmup_on   -w cpu_int,cpu_fp,cpu_hash -t 1 -n 200 --trial-ms 50 --warmup 20 --warmup-ms 500 --cold clflush
run knob_warmup_off  -w cpu_int,cpu_fp,cpu_hash -t 1 -n 200 --trial-ms 50 --warmup 0  --warmup-ms 0   --spin-ms 0 --cold clflush

# ---- knob: trial length -------------------------------------------------------------
for ms in 10 50 200; do
  run "knob_trialms_$ms" -w cpu_int,cpu_fp,cpu_hash -t 1 -n 200 --trial-ms $ms --warmup 20 --cold clflush
done

# ---- interleave vs sequential (same configs, same trial count) ----------------------
INT="-w cpu_int,cpu_fp,cpu_hash -t 1 -n 300 --trial-ms 50 --warmup 20 --cold clflush"
run interleave_off $INT
run interleave_on  $INT --interleave

echo "finished $(date -Is)" | tee -a "$LOG"

# ---- appended: Target #2 retry at the trial length the knob study identified ----------
# The knob A/B found trial length is the one knob that clearly moves CoV, and that
# --trial-ms 200 reaches ~3% at 1 thread. Target #2 requires >= 1000 trials, so this
# re-tests it properly at that trial length.
