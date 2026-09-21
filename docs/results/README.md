# Measured results

Every number in this repository comes from a file in this directory. Nothing here is
typed by hand, estimated, or copied from a vendor spec sheet. Each subdirectory holds the
raw engine output for one milestone plus a README recording the exact commands and the
machine state at the time.

## The machine

Intel Core Ultra 9 185H (6 P-cores + 8 E-cores + 2 LP E-cores, 22 threads), 16 GB,
Ubuntu 22.04.5 under **WSL2** — so the 22 "CPUs" are Hyper-V vCPUs and the hybrid layout
is hidden from the guest. Every number below is a *virtual machine on a laptop* number.
See "Environment caveats" in `CLAUDE.md`.

Effective core frequency is **measured**, not read: WSL2 reports a synthetic 3072 MHz on
all 22 vCPUs and Windows reports only the 2.3 GHz nominal. Two dependent-instruction
chains of different known latencies agree on **~4.28 GHz** single-threaded
(`m2.4/frequency_probe.log`).

## CPU metrics — M2.4, 2026-09-20

Source: `cpu_2026-09-20.json` (= `m2.4/cpu_sweep_unpinned.json`). Median of 30 trials of
50 ms each, `--cold clflush`, unpinned. Full tables and the pinned comparison are in
`m2.4/README.md`.

Power state: **plugged in**, Windows plan **Balanced**, power mode **Best power
efficiency**, battery low and charging. Left as found, so these are floors.

| metric | best threads | aggregate | at 1 thread | ops/cycle at 1 thread | Target #1 (≥ 1e7) |
|---|---:|---:|---:|---:|:--:|
| `cpu_int_ops` | 16 | **37.1 G ops/s** | 3.86 G ops/s | 0.90 | **PASS** (×3711) |
| `cpu_fp_ops` | 22 | **85.9 G ops/s** | 7.91 G ops/s | 1.85 | **PASS** (×8587) |
| `cpu_hash_ops` | 16 | **1.08 G ops/s** | 0.128 G ops/s | 0.030 | **PASS** (×108) |

An "op" is defined exactly in `CLAUDE.md` — one integer lane update, one double-precision
FMA on one lane, one 64-byte block hashed. Those definitions are what make these numbers
comparable to anything; the raw magnitudes on their own are not interesting.

The `ops/cycle` column is the one worth reading, because each value was predicted from the
kernel's disassembly before it was measured, and all three predictions held to 7–8 %:
`cpu_int` is limited by the single integer-multiply port (1 op/cycle), `cpu_fp` by
8 lanes ÷ 4-cycle FMA latency (2 ops/cycle), `cpu_hash` by its 31 multiplies per block
(0.032 ops/cycle).

Scaling is strongly sub-linear past ~4 threads and peaks at 16 of 22 threads for two of
the three metrics — the hybrid P/E-core layout and SMT, seen from inside a VM that will
not say which is which. Guest-side pinning did not help (`--pin` was 6–9 % *slower* at the
best thread count).

## Index

| directory | milestone | what it holds |
|---|---|---|
| `m1.2/` | M1.2 | measurement fundamentals: warmup effect, trial-length scaling, debug-vs-release |
| `m2.1/` | M2.1 | worker pool: start spread, pinned vs unpinned scaling, false sharing |
| `m2.2/` | M2.2 | per-configuration summaries and the `--max-seconds` safety cap |
| `m2.3/` | M2.3 | cold-cache infrastructure: flush vs evict, THP, cold-mode A/B |
| `m2.4/` | M2.4 | the three CPU kernels, the thread sweep, Target #1, frequency probe |

## Targets

| target | status | evidence |
|---|---|---|
| Engine ≥ 10M ops/s | **met** (M2.4) | `cpu_2026-09-20.json`, table above |
| CoV ≤ 3 % over ≥ 1000 trials | not yet run | M2.5 (CPU), M3.3 (memory) |
| API ingests/queries ≥ 100K measurements | not yet run | M5.2 |
| API ≥ 1000 req/s, p95 ≤ 50 ms | not yet run | M5.4 |
| Dashboard: 12 metrics, 10K+ points | not yet run | M6.3 |

The CoV column in `m2.4/README.md` is *not* the Target #2 measurement: those are 30-trial
runs made while sweeping thread counts, and they range from 3.2 % to 15 %. Target #2 is
1000 trials of one configuration with the knobs set deliberately, and it is M2.5's job.
