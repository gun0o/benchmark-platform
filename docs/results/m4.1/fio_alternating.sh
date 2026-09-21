#!/usr/bin/env bash
# A single A-then-B comparison confounds the tool with when it ran, and this machine's
# storage has an uncontrollable variable in it: the Windows host's cache over the VHDX.
# M2.5 learned this the hard way for CPU knobs; the same rule applies here. Three
# alternating repetitions of engine-then-fio, same parameters, same file.
set -euo pipefail
cd "$(dirname "$0")"
BENCH=../../../engine/build/release/bench
FIO=${FIO:-$HOME/fio/fio}
FILE="$HOME/.cache/bench/testfile"
common=(--filename="$FILE" --direct=1 --ioengine=psync --iodepth=1 --time_based
        --runtime=5s --ramp_time=1s --group_reporting --output-format=json)

for rep in 1 2 3; do
  for t in 1 4; do
    "$BENCH" run -m disk_seq_read_bw -t $t -n 5 --trial-ms 500 --warmup 2 --warmup-ms 500 \
        --spin-ms 200 -o "alt_bench_seq_t${t}_$rep.json" 2>/dev/null
    if [ "$t" = 1 ]; then
      "$FIO" "${common[@]}" --name=s --rw=read --bs=1M --numjobs=1 --size=4G \
          > "alt_fio_seq_t1_$rep.json"
    else
      "$FIO" "${common[@]}" --name=s --rw=read --bs=1M --numjobs=4 --size=1G \
          --offset_increment=1G > "alt_fio_seq_t4_$rep.json"
    fi
  done
  for t in 1 4 16; do
    "$BENCH" run -m disk_rand_read_iops -t $t -n 5 --trial-ms 500 --warmup 2 --warmup-ms 500 \
        --spin-ms 200 -o "alt_bench_rand_t${t}_$rep.json" 2>/dev/null
    "$FIO" "${common[@]}" --name=r --rw=randread --bs=4k --numjobs=$t --size=4G \
        > "alt_fio_rand_t${t}_$rep.json"
  done
done
