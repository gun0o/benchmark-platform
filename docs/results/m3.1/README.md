# M3.1 memory bandwidth — raw runs (2026-09-21)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`). 48 KiB L1d, 2 MiB L2 per
core, 24 MiB L3, 16 GB.

**Power state is recorded before and after every run** in `run_log.txt`, by
`powerstate.sh`. Every run here was taken **plugged in**, battery 100 %, Windows plan
**Balanced**, power mode **Best performance** ("Max Performance Overlay"). M2.4 measured
that the power mode alone is worth 8–17 % of throughput and a 4× change in CoV, so a run
that did not record it would not be comparable to anything.

Conclusions: `docs/notes/M3.1.md`. Reader-facing caveats: `docs/methodology.md`.

## Files

| file | what it is |
|---|---|
| `run_study.sh` | every Verify run in PLAN.md M3.1, in order, with the power state around each |
| `run_log.txt` | the commands, the power state and the load averages, as they happened |
| `powerstate.sh` | prints power line / battery / Windows power mode in one line |
| `analyze.py`, `analysis.md` | the tables quoted in the notes, read straight out of the `summary` arrays |
| `sweep_ws_1t.json` / `.txt` | Verify 1: working-set sweep 8 KiB → 256 MiB, 1 thread, all three metrics |
| `sweep_threads_read.json` / `.txt` | Verify 2: `mem_read_bw` at 1–22 threads, 256 MiB per thread |
| `dram_rwc_{1,8}t{,_nt}.json` / `.txt` | Verify 3: read/write/copy at 256 MiB, plain vs `--nt` |
| `nt_threshold.sh`, `nt_threshold.txt`, `nt_threshold_{default,forced}.json` | whether glibc's `memcpy` was already using NT stores |
| `disasm_mem_bw.log` | Verify 4: `objdump` of `run_batch()`, showing the read loop's `vpaddq` on `ymm` |

## Settings common to the measurement runs

```
--trials 10 --trial-ms 50 --warmup 5 --warmup-ms 300 --spin-ms 500 --pin --cold clflush
```

10 trials of 50 ms is half a second of timed measurement per configuration on top of the
warmup floor: enough for a plateau to be visible, **not** enough to support a CoV claim.
Variance for the memory metrics is M3.3's job and is not attempted here. The CoV column in
the `.txt` tables is there because `bench report` prints it, not as a result.

## Headline numbers (medians, GB/s, 1 GB = 1e9 bytes)

| | read | write | copy |
|---|---:|---:|---:|
| L1-resident (32 KiB, 1 thread) | 253.7 | 290.9 | — (copy needs 2×, see notes) |
| DRAM (256 MiB, 1 thread) | 22.9 | 14.1 | 21.4 |
| DRAM (256 MiB, 16 threads) | **84.2** | — | — |
| DRAM (256 MiB, 8 threads, `--nt`) | 66.1 | 59.5 | 41.9 |

Read is 11× faster from L1 than from DRAM. The write cliff at the L1 boundary is 3.6×
against read's 1.3× — that gap is write-allocate, and it is the clearest single measurement
in this milestone.

## Reproducing

```bash
cd engine && cmake --build --preset release -j
cd ../docs/results/m3.1 && ./run_study.sh && ./nt_threshold.sh && python3 analyze.py
```

Runtime is about 90 seconds of measurement. Every `.json` here passes both
`bench validate` and `check-jsonschema --schemafile schema/benchmark-result.schema.json`.
