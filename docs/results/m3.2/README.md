# M3.2 cache latency — raw runs (2026-09-21)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`). 48 KiB L1d, 2 MiB L2 per
core, 24 MiB L3, 16 GB. THP policy `madvise`.

**Power state is recorded before and after every run** in `run_log.txt`, by
`powerstate.sh`. Every run here was taken **plugged in**, battery 100 %, Windows plan
**Balanced**, power mode **Best performance** ("Max Performance Overlay").

Conclusions: `docs/notes/M3.2.md`. Reader-facing caveats: `docs/methodology.md`.

## Why the sweep is run five times

Under WSL2 a thread's physical core is not determined by its vCPU, and on this hybrid part
the path to the last-level cache differs between core types. An 8 MiB chase measured
17.0, 17.2, 18.6, 19.3, 29.0, 35.9, 107.5 and 129.9 ns across eight separate runs — two
clusters, not one distribution. A single pass reports whichever core it landed on. Five
passes in five separate processes report the distribution, and `analysis.md` prints every
pass rather than only the aggregate.

## Files

| file | what it is |
|---|---|
| `run_study.sh` | every Verify run in PLAN.md M3.2, with the power state around each |
| `run_log.txt` | the commands, power state, load averages and THP policy, as they happened |
| `powerstate.sh` | prints power line / battery / Windows power mode in one line |
| `analyze.py`, `analysis.md` | the tables quoted in the notes, read out of the `summary` arrays |
| `sweep_rep{1..5}.json` | Verify 1: 4 KiB → 1 GiB, 1 thread, warm, five independent passes |
| `huge_rep{1..3}.json`, `nohuge_rep{1..3}.json` | Verify 2: `--no-hugepages`, three alternating pairs |
| `cov_1m_{cold,warm}.json` | Verify 3: 1 MiB, 200 trials of 50 ms, `--cold clflush` vs `none` |
| `coldmode_{clflush,none}.json` | what cold mode does to a chase across working sets |
| `disasm_mem_latency.log` | `objdump` of `run_batch()`: one `mov (%rcx,%rdx,8),%rdx` per op |
| `ctest_presets.log` | 152/152 in release, debug, ci, asan, tsan (and one recorded flake) |

## Headline numbers (ns per dependent load, 1 thread, warm)

| working set | ns/load | what it is |
|---|---:|---|
| 4 KiB | **1.02** | L1d — 4.9 cycles at the 4.83 GHz M2.4 measured |
| 48 KiB | 1.10 | L1d, exactly at capacity |
| 64 KiB | **3.25** | the L1→L2 step, at the size `sysinfo` reports |
| 2 MiB | 6.07 | L2, at capacity |
| 8 MiB | 46.01 | **bimodal**, 28–107 across passes; not a usable L3 figure |
| 24 MiB | 146.40 | DRAM |
| 1 GiB | **175.78** | DRAM, +20 % over 24 MiB from address-span effects, not TLB |

The L1 step is the cleanest result: a 3× jump between two adjacent sweep points, landing
on a cache size nothing told the benchmark about. The L3 region has no plateau at all —
see the notes.

## Reproducing

```bash
cd engine && cmake --build --preset release -j
cd ../docs/results/m3.2 && ./run_study.sh && python3 analyze.py
```

About 107 seconds of measurement. Every `.json` here passes both `bench validate` and
`check-jsonschema --schemafile schema/benchmark-result.schema.json`.
