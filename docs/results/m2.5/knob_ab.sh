#!/usr/bin/env bash
# M2.5 knob A/B, second pass.
#
# The first pass ran each knob setting once, one after another. That confounds the knob
# with WHEN the run happened, and this machine drifts ~10% across a minute of load, so a
# single A-then-B comparison cannot separate "the knob did it" from "the package was
# warmer by then". This pass alternates the settings and repeats, the same way the M2.4
# hash-fold study did, so the drift lands on both arms equally.
set -u
cd "$(dirname "$0")/../../.."
BIN=engine/build/release/bench
D=docs/results/m2.5/knobs
PS=docs/results/m2.5/powerstate.sh
mkdir -p "$D"
LOG=docs/results/m2.5/knob_ab_log.txt
: > "$LOG"
echo "knob A/B second pass, started $(date -Is)" | tee -a "$LOG"
echo "before: $($PS)  $(uptime | sed 's/.*load average/load/')" | tee -a "$LOG"

BASE="-w cpu_int,cpu_fp,cpu_hash -t 1 --warmup 20 --cold clflush"
for rep in 1 2 3; do
  $BIN run $BASE -n 100 --trial-ms 50 --pin  --out "$D/pin_on_$rep.json"  2>>"$LOG"
  $BIN run $BASE -n 100 --trial-ms 50        --out "$D/pin_off_$rep.json" 2>>"$LOG"
  $BIN run -w cpu_int,cpu_fp,cpu_hash -t 1 -n 100 --trial-ms 50 --warmup 20 --warmup-ms 500 --cold clflush \
        --out "$D/warmup_on_$rep.json"  2>>"$LOG"
  $BIN run -w cpu_int,cpu_fp,cpu_hash -t 1 -n 100 --trial-ms 50 --warmup 0 --warmup-ms 0 --spin-ms 0 --cold clflush \
        --out "$D/warmup_off_$rep.json" 2>>"$LOG"
  for ms in 10 50 200; do
    $BIN run $BASE -n 100 --trial-ms $ms --out "$D/trialms_${ms}_$rep.json" 2>>"$LOG"
  done
  echo "rep $rep done $(date -Is)  $($PS)  $(uptime | sed 's/.*load average/load/')" | tee -a "$LOG"
done
echo "after:  $($PS)  $(uptime | sed 's/.*load average/load/')" | tee -a "$LOG"
echo "finished $(date -Is)" | tee -a "$LOG"
