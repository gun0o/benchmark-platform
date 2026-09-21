# M4.1 disk I/O — raw runs (2026-09-21)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset. Test file: **4 GiB** at `$HOME/.cache/bench/testfile`, on ext4 on
`/dev/sde` — which is a **VHDX on the Windows host**, not a physical device. Guest
`MemAvailable` at the time: 13.4 GB, i.e. the guest could have cached the whole file, which
is why Verify 3 exists. Physical sector size 4096, logical 512. Full environment dump:
`environment.txt`.

**Power state is recorded before and after every run** in `run_log.txt`, by
`powerstate.sh`: **plugged in**, battery 100 %, Windows plan **Balanced**, power mode
**Best performance**.

Conclusions: `docs/notes/M4.1.md`. Reader-facing caveats: `docs/methodology.md`.

## These are "virtual disk" numbers

`O_DIRECT` bypasses the **guest's** page cache, and Verify 3 measures that it does (filling
the guest cache with the whole 4 GiB file changes the next run by −0.1 %). It says nothing
about whether Windows is caching the VHDX. That cannot be observed, controlled or ruled out
from inside the guest, so every figure here is labelled as what a program running in this
WSL2 guest sees — which is a useful thing to know and is not the device's number.

## Files

| file | what it is |
|---|---|
| `run_study.sh` | every Verify run in PLAN.md M4.1, in order, with the power state around each |
| `run_log.txt`, `environment.txt` | the commands and the machine state as they happened |
| `strace_check.sh`, `strace.log` | Verify 1: `O_DIRECT` flags, transfer sizes, offset alignment, the fdatasync ratio |
| `seq_read.json` / `.txt` | Verify 2: 1 MiB sequential reads at 1 and 4 threads |
| `rand_read.json` / `.txt` | Verify 2: 4 KiB random reads (and their p99) at QD 1, 4, 16 |
| `seq_write.json` / `.txt`, `rand_write.json` / `.txt` | the write metrics, `--allow-writes --write-budget 8G` |
| `fio_compare.sh`, `fio_*.json` | the fio reference run, same parameters |
| `fio_alternating.sh`, `alt_*.json` | the same comparison run alternating, three repetitions |
| `fio_sensitivity.txt` | how much fio's own answer moves with its own parameter choices |
| `cold_trial0.json`, `cold_warmcache.json`, `cold_pagecache.txt` | Verify 3: the cold check |
| `analyze.py`, `analysis.md` | every table quoted in the notes |
| `ctest_presets.log` | 168/168 in release, debug, ci, asan, tsan |

## Headline numbers

| metric | 1 thread | 4 threads | 16 threads |
|---|---:|---:|---:|
| `disk_seq_read_bw` | 2690 MB/s | **6405 MB/s** | — |
| `disk_seq_write_bw` | 1487 MB/s | 1951 MB/s | — |
| `disk_rand_read_iops` | 9131 | 32808 | **89966** |
| `disk_rand_read_p99_us` | 274 µs | 234 µs | 300 µs |
| `disk_rand_write_iops` | 512 | 554 | 585 |

Medians. Reads: 5 trials of 500 ms. Sequential writes: 3 trials of 300 ms (that is the SSD
wear). 1 MB = 1e6 bytes.

Random **writes** barely scale — 512 → 585 IOPS for sixteen times the threads — because
`O_DSYNC` makes every write durable before it returns and flushes serialize. A durable
4 KiB write costs about 2 ms here, 18× a 4 KiB read.

## Agreement with fio

Run the obvious way (engine, then fio), the engine looked 12–18 % faster. Run **alternating**
three times, as M2.5's lesson requires on a machine that drifts, every ratio is within
**±5 %**:

| measurement | median bench/fio | range |
|---|---:|---|
| seq read, 1 thread | 0.968 | 0.954–1.025 |
| seq read, 4 threads | 0.984 | 0.981–0.990 |
| rand read, QD 1 | 0.958 | 0.928–0.979 |
| rand read, QD 4 | 1.016 | 0.995–1.061 |
| rand read, QD 16 | 1.030 | 0.989–1.063 |

For scale: fio's own QD 1 answer moves between 7763 and 8815 IOPS depending only on which of
four reasonable parameter choices it is given (`fio_sensitivity.txt`).

## Reproducing

fio is not packaged here and there is no root, so it is built from source:

```bash
curl -sSL -o fio.tar.gz https://github.com/axboe/fio/archive/refs/tags/fio-3.36.tar.gz
tar xzf fio.tar.gz && cd fio-fio-3.36 && ./configure && make -j8
```

Then:

```bash
cd engine && cmake --build --preset release -j
cd ../docs/results/m4.1
./run_study.sh                      # ~50 s, writes ~8 GiB to the SSD
./strace_check.sh > strace.log
FIO=/path/to/fio ./fio_compare.sh
FIO=/path/to/fio ./fio_alternating.sh
python3 analyze.py > analysis.md
../../../engine/build/release/bench disk clean   # delete the 4 GiB test file
```

Every engine `.json` here passes both `bench validate` and
`check-jsonschema --schemafile schema/benchmark-result.schema.json`. The `fio_*.json` files
are fio's own output format and are not engine run documents.
