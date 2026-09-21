#!/usr/bin/env bash
# PLAN.md M4.1 Verify: "Compare with fio --direct=1 --ioengine=psync with the same
# parameters (fio is the reference tool; agreement within ~10% validates the engine).
# Commit both outputs."
#
# fio is not packaged on this machine and there is no root, so it is built from source:
#   curl -sSL -o fio.tar.gz https://github.com/axboe/fio/archive/refs/tags/fio-3.36.tar.gz
#   tar xzf fio.tar.gz && cd fio-fio-3.36 && ./configure && make -j8
# The psync engine is built in and needs no optional libraries. Point FIO at the binary.
#
# Matching the engine's parameters exactly is the whole point of the comparison:
#   --direct=1 --ioengine=psync  same syscalls (pread/pwrite) and O_DIRECT
#   --bs=1M / --bs=4k            the engine's block sizes
#   --numjobs=N --iodepth=1      the engine's "one outstanding I/O per thread, QD = threads"
#   --offset_increment / --size  for sequential, one contiguous region per job, as the
#                                engine splits the file
#   --time_based --runtime=5s    the engine's 5 x 500 ms of timed trials
set -euo pipefail
cd "$(dirname "$0")"
FIO=${FIO:-$HOME/fio/fio}
FILE="$HOME/.cache/bench/testfile"
[ -x "$FIO" ] || { echo "fio not found at $FIO; set FIO=/path/to/fio" >&2; exit 1; }
"$FIO" --version

common=(--filename="$FILE" --direct=1 --ioengine=psync --iodepth=1 --time_based
        --runtime=5s --ramp_time=1s --group_reporting --output-format=json)

run() { # run <name> <extra fio args...>
  local name=$1; shift
  echo "### $name" >&2
  "$FIO" "${common[@]}" --name="$name" "$@" > "fio_$name.json"
}

# Sequential read, 1 and 4 jobs. Each job gets its own contiguous quarter at 4 jobs, which
# is how the engine splits the file: N jobs are N sequential streams, not one interleaved.
run seqread_t1  --rw=read --bs=1M --numjobs=1 --size=4G
run seqread_t4  --rw=read --bs=1M --numjobs=4 --size=1G --offset_increment=1G

# Random read over the whole file, 1 / 4 / 16 jobs = queue depth 1 / 4 / 16.
for n in 1 4 16; do
  run "randread_t$n" --rw=randread --bs=4k --numjobs="$n" --size=4G
done
