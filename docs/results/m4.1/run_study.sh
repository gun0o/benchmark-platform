#!/usr/bin/env bash
# M4.1 verification runs. Every Verify bullet in PLAN.md M4.1, in order.
#
# Environment facts that change what these numbers mean are recorded up front: the guest's
# MemAvailable against the test file's size (the guest could cache the whole file, which is
# why O_DIRECT has to be checked rather than assumed), and the Windows power state.
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
FILE="$HOME/.cache/bench/testfile"
SIZE=4G
LOG=run_log.txt
: > "$LOG"

power() { printf '%s  %s  %s\n' "$(date -Is)" "$1" "$(bash ./powerstate.sh)" | tee -a "$LOG"; }
note()  { printf '%s\n' "$*" | tee -a "$LOG"; }

{
  echo "=== environment ==="
  date -Is
  uname -a
  echo "--- root filesystem (the test file lives on it) ---"
  df -hT / ; mount | grep ' / '
  echo "--- device sector sizes ---"
  lsblk -o NAME,SIZE,PHY-SEC,LOG-SEC "$(findmnt -no SOURCE /)" || true
  echo "--- guest memory: MemAvailable vs the test file ---"
  grep -E 'MemTotal|MemAvailable' /proc/meminfo
  echo "test file size: $SIZE"
  echo "--- uptime / load ---"
  uptime
} | tee -a "$LOG" > environment.txt

power "before prep"
note "+ bench disk prep --size $SIZE"
"$BENCH" disk prep --size "$SIZE" | tee -a "$LOG"
power "after prep"

# Verify 1: strace. Does the engine actually open O_DIRECT, and are the transfers 1 MiB and
# 4 KiB? -c gives the call counts; the -e verbose run shows the flags and one size each.
note "=== Verify 1: strace ==="
{
  echo "# openat flags as the engine issues them (O_DIRECT = 0x4000 = 16384)."
  strace -f -e trace=openat -e status=successful \
    "$BENCH" run -m disk_seq_read_bw -n 1 --trial-ms 50 --warmup 0 --warmup-ms 0 --spin-ms 0 \
    -o /dev/null 2>&1 | grep -E 'testfile' || true
  echo
  echo "# 1 MiB sequential reads: pread64 count and sizes."
  strace -f -c -e trace=pread64,fdatasync,openat \
    "$BENCH" run -m disk_seq_read_bw -n 1 --trial-ms 50 --warmup 0 --warmup-ms 0 --spin-ms 0 \
    -o /dev/null 2>&1 | tail -12
  echo
  echo "# one pread64 line each, showing the transfer size (1048576 seq, 4096 rand)."
  strace -f -e trace=pread64 \
    "$BENCH" run -m disk_seq_read_bw -n 1 --trial-ms 20 --warmup 0 --warmup-ms 0 --spin-ms 0 \
    -o /dev/null 2>&1 | grep -m2 'pread64' || true
  strace -f -e trace=pread64 \
    "$BENCH" run -m disk_rand_read_iops -n 1 --trial-ms 20 --warmup 0 --warmup-ms 0 --spin-ms 0 \
    -o /dev/null 2>&1 | grep -m2 'pread64' || true
} > strace.log 2>&1
tail -6 strace.log | tee -a "$LOG"

# Verify 2: the numbers. Reads first (no wear), then the gated write metrics.
COMMON="--trial-ms 500 --warmup 2 --warmup-ms 500 --spin-ms 300"
power "before reads"
note "+ disk_seq_read_bw at 1,4 threads"
# shellcheck disable=SC2086
"$BENCH" run -m disk_seq_read_bw -t 1,4 -n 5 $COMMON -o seq_read.json 2>>"$LOG"
"$BENCH" report seq_read.json --wide | tee seq_read.txt | tee -a "$LOG"
note "+ disk_rand_read_iops (and its p99) at 1,4,16 threads"
# shellcheck disable=SC2086
"$BENCH" run -m disk_rand_read_iops -t 1,4,16 -n 5 $COMMON -o rand_read.json 2>>"$LOG"
"$BENCH" report rand_read.json --wide | tee rand_read.txt | tee -a "$LOG"
power "after reads"

# Write metrics. --allow-writes is required; the budget is raised from its 1 GiB default and
# the raised value is recorded in the run's argv.
power "before writes"
note "+ disk_seq_write_bw at 1,4 threads (shorter trials: this is the wear)"
"$BENCH" run -m disk_seq_write_bw -t 1,4 -n 3 --trial-ms 300 --warmup 1 --warmup-ms 300 \
    --spin-ms 300 --allow-writes --write-budget 8G -o seq_write.json 2>>"$LOG"
"$BENCH" report seq_write.json --wide | tee seq_write.txt | tee -a "$LOG"
note "+ disk_rand_write_iops at 1,4,16 threads (O_DSYNC, slow, writes little)"
# shellcheck disable=SC2086
"$BENCH" run -m disk_rand_write_iops -t 1,4,16 -n 5 $COMMON --allow-writes --write-budget 8G \
    -o rand_write.json 2>>"$LOG"
"$BENCH" report rand_write.json --wide | tee rand_write.txt | tee -a "$LOG"
power "after writes"

# Verify 3: the cold check. If O_DIRECT is bypassing the guest page cache, then (a) the
# first trial is no slower than the rest, and (b) deliberately filling the page cache with
# the whole file beforehand does not make the next run faster.
note "=== Verify 3: cold check ==="
power "before cold check"
"$BENCH" run -m disk_seq_read_bw -t 1 -n 10 --trial-ms 300 --warmup 0 --warmup-ms 0 \
    --spin-ms 300 -o cold_trial0.json 2>>"$LOG"
note "+ filling the guest page cache with the whole file, then re-running"
{ echo "--- free before cat ---"; free -m
  dd if="$FILE" of=/dev/null bs=1M status=none
  echo "--- free after cat (buff/cache should have grown by ~4 GiB) ---"; free -m
} | tee -a "$LOG" > cold_pagecache.txt
"$BENCH" run -m disk_seq_read_bw -t 1 -n 10 --trial-ms 300 --warmup 0 --warmup-ms 0 \
    --spin-ms 300 -o cold_warmcache.json 2>>"$LOG"
power "after cold check"

note "=== done ==="
free -m | tee -a "$LOG"
