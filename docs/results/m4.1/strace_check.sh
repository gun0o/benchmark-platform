#!/usr/bin/env bash
# PLAN.md M4.1 Verify: "strace -e trace=openat,pread64 -c confirms O_DIRECT is set and
# reads are 4096/1 MiB."
#
# The raw pread64 stream also contains the dynamic loader reading ELF headers out of
# libstdc++, which is why the sizes are counted rather than eyeballed: what matters is that
# every read the *benchmark* issues is the block size its metric claims.
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
Q="--warmup 0 --warmup-ms 0 --spin-ms 0 -o /dev/null"

sizes() { # sizes <syscall> -- prints "count length" for each distinct transfer length
  grep -oE "$1\([0-9]+, .*, [0-9]+, [0-9]+\) = [0-9]+" \
    | sed -E "s/^$1\([0-9]+, .*, ([0-9]+), ([0-9]+)\) = ([0-9]+)$/\1 \2 \3/" \
    | awk '{print $1}' | sort -n | uniq -c
}
offsets_mod() { # how many of the offsets are 4096-aligned
  grep -oE "pread64\([0-9]+, .*, 4096, [0-9]+\) = 4096" \
    | sed -E 's/.*, 4096, ([0-9]+)\) = 4096/\1/' \
    | awk '{print ($1 % 4096)}' | sort -n | uniq -c
}

echo "=== 1. Is O_DIRECT actually set on the test file? ==="
strace -f -e trace=openat -e status=successful \
  $BENCH run -m disk_seq_read_bw -n 1 --trial-ms 20 $Q 2>&1 | grep testfile
strace -f -e trace=openat -e status=successful \
  $BENCH run -m disk_rand_write_iops -n 1 --trial-ms 20 --allow-writes --write-budget 64M $Q \
  2>&1 | grep testfile

echo
echo "=== 2. disk_seq_read_bw: transfer sizes (count, bytes) ==="
strace -f -e trace=pread64 $BENCH run -m disk_seq_read_bw -n 1 --trial-ms 20 $Q 2>&1 | sizes pread64

echo
echo "=== 3. disk_rand_read_iops: transfer sizes (count, bytes) ==="
strace -f -e trace=pread64 $BENCH run -m disk_rand_read_iops -n 1 --trial-ms 20 $Q 2>&1 | sizes pread64
echo "--- and offset mod 4096 (count, remainder): all must be 0 ---"
strace -f -e trace=pread64 $BENCH run -m disk_rand_read_iops -n 1 --trial-ms 20 $Q 2>&1 | offsets_mod

echo
echo "=== 4. disk_seq_write_bw: pwrite64 sizes, and one fdatasync per 16-block batch ==="
strace -f -e trace=pwrite64 \
  $BENCH run -m disk_seq_write_bw -n 1 --trial-ms 100 --allow-writes --write-budget 512M $Q \
  2>&1 | sizes pwrite64
strace -f -c -e trace=pwrite64,fdatasync \
  $BENCH run -m disk_seq_write_bw -n 1 --trial-ms 100 --allow-writes --write-budget 512M $Q \
  2>&1 | tail -7

echo
echo "=== 5. call counts for a sequential read trial ==="
strace -f -c -e trace=openat,pread64 $BENCH run -m disk_seq_read_bw -n 1 --trial-ms 50 $Q 2>&1 | tail -7
