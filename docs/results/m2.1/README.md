# M2.1 verification runs (2026-09-18)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`), plugged in, postgres/redis
containers idle. Defaults in force: `--warmup 5 --warmup-ms 500 --spin-ms 500`.
Interpretation is in `docs/notes/M2.1.md`.

| file | command (from `engine/`) |
|---|---|
| `spread_8t_100.json` | `bench run -w cpu_int -t 8 -n 100 --trial-ms 20` |
| `spread_8t_100_pinned.json` | `bench run -w cpu_int -t 8 -n 100 --trial-ms 20 --pin` |
| `pin_8t.json`, `pin_8t.log` | `bench run -w cpu_int -t 8 -n 10 --trial-ms 20 --pin -v` (log = stderr) |
| `scaling_pinned.json` | `bench run -w cpu_int -t 1,2,4,6,8,11,16,22 -n 20 --trial-ms 50 --pin` |
| `scaling_unpinned.json` | `bench run -w cpu_int -t 1,2,4,6,8,11,16,22 -n 20 --trial-ms 50` |
| `scaling_table.json` | derived: per thread count, median aggregate ops/s, efficiency, CoV, start-spread p95/max |

Per-trial fields used: `params.start_spread_us` (latest minus earliest worker start),
`params.release_to_first_start_us`, `params.per_thread_ops_per_s`, `params.per_thread_cpu`,
`params.pin_violations`, `params.warmups_run`, `params.clock_call_ns`.

The perf tests print their own numbers: `build/release/tests/false_sharing_test` and
`build/release/tests/barrier_sync_test` (run them alone; they are label `perf` in ctest).
