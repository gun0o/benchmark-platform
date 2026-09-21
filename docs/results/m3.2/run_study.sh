#!/usr/bin/env bash
# M3.2 verification runs, with the power state recorded around each one.
#
# The sweep is repeated five times in five separate processes rather than run once. Under
# WSL2 a thread's physical core is not determined by its vCPU (M2.1), and on a hybrid part
# the path to the last-level cache differs between core types: an 8 MiB chase measured
# 17.0-129.9 ns across eight repetitions. One pass would report whichever core it landed
# on. Five passes report the distribution, which is the honest object.
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
LOG=run_log.txt
: > "$LOG"

power() { printf '%s  %s  %s\n' "$(date -Is)" "$1" "$(bash ./powerstate.sh)" | tee -a "$LOG"; }
note()  { printf '%s\n' "$*" | tee -a "$LOG"; }

# PLAN.md's sweep. --cold none: this is the steady-state latency of each cache level, so
# the chase must be allowed to warm up. The cold-vs-warm comparison is a separate run below.
WS=4K,16K,32K,48K,64K,128K,512K,1M,2M,4M,8M,16M,24M,32M,64M,256M,1G
COMMON="--trials 10 --trial-ms 30 --warmup 5 --warmup-ms 200 --spin-ms 300"

note "=== uptime before ==="; uptime | tee -a "$LOG"
note "=== free before ===";   free -m | tee -a "$LOG"
note "=== THP policy ==="; cat /sys/kernel/mm/transparent_hugepage/enabled | tee -a "$LOG"

# Verify 1: the working-set sweep, five independent passes.
for rep in 1 2 3 4 5; do
  power "before sweep rep $rep"
  note "+ bench run -w mem_latency --working-set $WS $COMMON --cold none -o sweep_rep$rep.json"
  # shellcheck disable=SC2086
  "$BENCH" run -w mem_latency --working-set "$WS" $COMMON --cold none -o "sweep_rep$rep.json" 2>>"$LOG"
  power "after sweep rep $rep"
done

# Verify 3: huge pages off. The large points should rise, because a 1 GiB chase over 4 KiB
# pages misses the TLB on every load as well as the cache.
for rep in 1 2 3; do
  power "before nohuge rep $rep"
  # shellcheck disable=SC2086
  "$BENCH" run -w mem_latency --working-set 8M,64M,256M,1G $COMMON --cold none --no-hugepages \
      -o "nohuge_rep$rep.json" 2>>"$LOG"
  # shellcheck disable=SC2086
  "$BENCH" run -w mem_latency --working-set 8M,64M,256M,1G $COMMON --cold none \
      -o "huge_rep$rep.json" 2>>"$LOG"
  power "after nohuge rep $rep"
done

# Verify 2: CoV at 1 MiB with --cold clflush, and the warm comparison. 200 trials, NOT the
# >=1000 Target #2 measurement - that is M3.3, which is out of scope here.
power "before cov_1m"
"$BENCH" run -w mem_latency --working-set 1M --trials 200 --trial-ms 50 --warmup 20 \
    --warmup-ms 500 --spin-ms 500 --pin --cold clflush -o cov_1m_cold.json 2>>"$LOG"
"$BENCH" run -w mem_latency --working-set 1M --trials 200 --trial-ms 50 --warmup 20 \
    --warmup-ms 500 --spin-ms 500 --pin --cold none -o cov_1m_warm.json 2>>"$LOG"
power "after cov_1m"

# What cold mode does to a latency chase at several sizes: unlike a bandwidth metric, here
# the cache state is the measurement rather than a transient.
power "before cold_vs_warm"
for mode in clflush none; do
  # shellcheck disable=SC2086
  "$BENCH" run -w mem_latency --working-set 256K,1M,8M,64M $COMMON --cold $mode \
      -o "coldmode_$mode.json" 2>>"$LOG"
done
power "after cold_vs_warm"

note "=== uptime after ==="; uptime | tee -a "$LOG"
note "=== free after ===";   free -m | tee -a "$LOG"
