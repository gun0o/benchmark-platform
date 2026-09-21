#!/usr/bin/env bash
# M3.1 verification runs. Every Verify bullet in PLAN.md M3.1, in order, with the power
# state recorded before and after each one (M2.4 measured 8-17% throughput and a 4x change
# in CoV between two Windows power modes, so a run without it is not comparable to anything).
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
LOG=run_log.txt
: > "$LOG"

power() { printf '%s  %s  %s\n' "$(date -Is)" "$1" "$(bash ./powerstate.sh)" | tee -a "$LOG"; }
note()  { printf '%s\n' "$*" | tee -a "$LOG"; }

# Common knobs. 10 trials at 50 ms is 0.5 s of timed measurement per configuration on top
# of the 0.5 s warmup floor: enough for a plateau to be visible, not enough for a CoV claim
# (that is M3.3's job, and M3.3 is out of scope here).
COMMON="--trials 10 --trial-ms 50 --warmup 5 --warmup-ms 300 --spin-ms 500 --pin --cold clflush"

run() { # run <name> <extra args...>
  local name=$1; shift
  power "before $name"
  note "+ bench run $* $COMMON -o $name.json"
  # shellcheck disable=SC2086
  "$BENCH" run "$@" $COMMON -o "$name.json" 2>>"$LOG"
  "$BENCH" report "$name.json" --wide | tee "$name.txt" | tee -a "$LOG"
  power "after $name"
}

note "=== uptime before ==="; uptime | tee -a "$LOG"
note "=== free before ===";   free -m | tee -a "$LOG"

# Verify 1: working-set sweep at 1 thread. Plateaus at L1 (48 KiB), L2 (2 MiB),
# L3 (24 MiB) and DRAM should be visible in all three metrics.
run sweep_ws_1t -w mem_bw -t 1 \
    --working-set 8K,16K,32K,64K,128K,256K,512K,1M,2M,4M,8M,16M,32M,64M,256M

# Verify 2: thread sweep at 256 MiB, read only. Where the curve flattens is the point at
# which the memory controller, not the cores, is the limit.
run sweep_threads_read -m mem_read_bw -t 1,2,3,4,6,8,11,16,22 --working-set 256M

# Verify 3: write vs read in the DRAM regime, with and without non-temporal stores.
run dram_rwc_1t   -w mem_bw -t 1  --working-set 256M
run dram_rwc_1t_nt   -w mem_bw -t 1  --working-set 256M --nt
run dram_rwc_8t   -w mem_bw -t 8  --working-set 256M
run dram_rwc_8t_nt   -w mem_bw -t 8  --working-set 256M --nt

note "=== uptime after ==="; uptime | tee -a "$LOG"
note "=== free after ===";   free -m | tee -a "$LOG"
