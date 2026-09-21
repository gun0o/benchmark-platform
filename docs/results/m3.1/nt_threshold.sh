#!/usr/bin/env bash
# Is glibc's memcpy already using non-temporal stores at 256 MiB?
#
# memcpy switches to NT stores above glibc.cpu.x86_non_temporal_threshold. If it is doing
# that, then "copy without --nt" is not the plain-store baseline the Verify step assumes,
# and copy's number cannot be compared with write's on the write-allocate argument.
# Raising the tunable above the working set forces the plain-store path and settles it.
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
C="--trials 10 --trial-ms 50 --warmup 5 --warmup-ms 300 --spin-ms 500 --pin --cold clflush"
{
  echo "=== glibc ==="; ldd --version | head -1
  echo "=== default threshold (getconf L3) ==="; getconf LEVEL3_CACHE_SIZE
  echo "=== tunable as seen by the loader ==="
  GLIBC_TUNABLES=glibc.cpu.x86_non_temporal_threshold=1073741824 \
    ld.so --list-tunables 2>/dev/null | grep non_temporal || echo "(ld.so --list-tunables unavailable)"
} | tee nt_threshold.txt

for tune in default forced; do
  if [ "$tune" = default ]; then unset GLIBC_TUNABLES || true
  else export GLIBC_TUNABLES=glibc.cpu.x86_non_temporal_threshold=1073741824; fi
  echo "--- memcpy NT threshold: $tune ---" | tee -a nt_threshold.txt
  # shellcheck disable=SC2086
  $BENCH run -m mem_copy_bw -t 1 --working-set 8M,256M $C -o "nt_threshold_$tune.json" 2>/dev/null
  $BENCH report "nt_threshold_$tune.json" | tail -n +3 | tee -a nt_threshold.txt
done
