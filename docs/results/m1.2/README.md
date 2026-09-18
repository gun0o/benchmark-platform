# M1.2 verification runs (2026-09-18)

Machine: Intel Core Ultra 9 185H, WSL2 (Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`), plugged in, Windows
"Balanced" power plan, postgres/redis containers idle. Interpretation is in
`docs/notes/M1.2.md`.

| file | command (from `engine/`) |
|---|---|
| `tm50.json`, `tm100.json` | `bench run -w cpu_int -n 10 --trial-ms 50` / `--trial-ms 100` |
| `rel.json` | `build/release/bench run -w cpu_int -n 5` |
| `dbg.json` | `build/debug/bench run -w cpu_int -n 5` |
| `nowarm50.json`, `nowarm5.json` | `bench run -w cpu_int -n 12 --warmup 0 --spin-ms 0 --trial-ms 50` / `5` |
| `ab_nowarm{1,2,3}.json` | `bench run -w cpu_int -n 12 --warmup 0 --spin-ms 0 --trial-ms 50` (3 repeats) |
| `ab_warm{1,2,3}.json` | `bench run -w cpu_int -n 12 --warmup 5 --spin-ms 500 --trial-ms 50` (3 repeats) |
| `vcpu_map.txt` | for c in 0..21: `taskset -c $c bench run -w cpu_int -n 3 --warmup 2 --spin-ms 100 --trial-ms 20`, median ops/s |
| `run_batch.asm` | `objdump -d --no-show-raw-insn -C build/release/bench`, `CpuIntWorkload::run_batch` only |

Each JSON is a full run envelope; `params.clock_call_ns` records the clock self-test for
that run, and `params.ops` / `params.batches` the work done per trial.
