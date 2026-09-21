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
chains of different known latencies agree on **~4.83 GHz** single-threaded under Best
performance (`m2.4/frequency_probe.log`), and ~4.28 GHz under Best power efficiency.

## CPU metrics — M2.4, 2026-09-20

Source: `cpu_2026-09-20.json` (= `m2.4/cpu_sweep_unpinned.json`). Median of 30 trials of
50 ms each, `--cold clflush`, unpinned. Full tables and the pinned comparison are in
`m2.4/README.md`.

Power state: **plugged in**, Windows plan **Balanced**, power mode **Best performance**
("Max Performance Overlay"), battery charging.

| metric | best threads | aggregate | at 1 thread | ops/cycle at 1 thread | Target #1 (≥ 1e7) |
|---|---:|---:|---:|---:|:--:|
| `cpu_int_ops` | 16 | **40.1 G ops/s** | 4.65 G ops/s | 0.96 | **PASS** (×4011) |
| `cpu_fp_ops` | 22 | **100.0 G ops/s** | 9.07 G ops/s | 1.88 | **PASS** (×10000) |
| `cpu_hash_ops` | 16 | **1.24 G ops/s** | 0.154 G ops/s | 0.032 | **PASS** (×124) |

The same sweep was run first under the **Best power efficiency** power mode and is kept
for comparison. Switching to Best performance was worth **+8 % to +17 %** on throughput,
+13 % on the measured clock, and cut single-thread CoV from 8–15 % to 2–4 %. A
power-managed laptop does not merely run slower; it runs unevenly.

An "op" is defined exactly in `CLAUDE.md` — one integer lane update, one double-precision
FMA on one lane, one 64-byte block hashed. Those definitions are what make these numbers
comparable to anything; the raw magnitudes on their own are not interesting.

The `ops/cycle` column is the one worth reading, because each value was predicted from the
kernel's disassembly before it was measured, and all three predictions held to within 6 %:
`cpu_int` is limited by the single integer-multiply port (1 op/cycle), `cpu_fp` by
8 lanes ÷ 4-cycle FMA latency (2 ops/cycle), `cpu_hash` by its 31 multiplies per block
(0.032 ops/cycle). Under Best power efficiency all three fell 7–10 % short in the same
direction; raising the power mode closed the gap to 4–6 %, which is the stated explanation
(sustained clock below the probe's peak) confirmed by an independent change.

Scaling is strongly sub-linear past ~4 threads and peaks at 16 of 22 threads for two of
the three metrics — the hybrid P/E-core layout and SMT, seen from inside a VM that will
not say which is which. Pinning made no significant difference under Best performance
(all three within 1.6 standard errors); under Best power efficiency it was 6–9 % *slower*
for two of the three. See `m2.4/README.md`.

## Memory bandwidth — M3.1, 2026-09-21

Source: `m3.1/`. Medians of 10 trials of 50 ms each, pinned, `--cold clflush`, 1 GB = 1e9
bytes. Same power state as above. Full tables: `m3.1/analysis.md` and `m3.1/README.md`;
the reasoning is in `docs/notes/M3.1.md` and the caveats in `docs/methodology.md`.

| metric | L1 (32 KiB, 1 thread) | DRAM (256 MiB, 1 thread) | DRAM, best threads |
|---|---:|---:|---:|
| `mem_read_bw` | **253.7 GB/s** | 22.9 GB/s | **84.2 GB/s** at 16 threads |
| `mem_write_bw` | **290.9 GB/s** | 14.1 GB/s | 41.4 GB/s at 8 threads (59.5 with `--nt`) |
| `mem_copy_bw` | — (a copy allocates 2×, so it leaves L1 one step earlier) | 21.4 GB/s | 41.9 GB/s at 8 threads |

Reads are **11× faster from L1 than from DRAM**, and the working-set sweep puts the steps
where `sysinfo` says the caches are. The largest step is L2→L3 (2.5×), not L1→L2 (1.3×):
a streaming read is close to the best case for this part's wide L2.

The sharpest single feature is **write-allocate**. At the 48 KiB L1d boundary, write
bandwidth falls 290.9 → 80.6 GB/s — a **3.6× cliff** — while read falls only 1.3×. A store
of 8 bytes to a 64-byte line that is not resident has to fetch the line first. The
rule-of-thumb "write ≈ ½ read" that follows is directionally right and numerically wrong
here: measured **0.61–0.63**, because at low thread counts the *read* baseline is the
core's outstanding-miss concurrency limit rather than a bandwidth ceiling. `--nt` closes
the ratio to 0.90 at 8 threads and overshoots to 1.88 at 1 thread.

One caveat carries into every copy number: **glibc's `memcpy` already uses non-temporal
stores above ~24 MiB on this machine**, tested by forcing the tunable (−12.2 % at 256 MiB,
unchanged at 8 MiB). `mem_copy_bw` and `mem_write_bw` are not on the same store path at
DRAM-sized working sets.

`mem_read_bw` at 22 threads reports 64.0 GB/s, *below* its 16-thread 84.2, with a worker
more than 1 ms late off the start barrier in 100 % of trials — there is no vCPU left for
the coordinator. It is published with its `late_trials` count, not dropped.

No CoV claim is made from these runs: 10 trials is enough to draw a curve, not to state a
variance. That is M3.3.

## Cache latency — M3.2, 2026-09-21

Source: `m3.2/`. Nanoseconds per dependent load in a random cyclic pointer chase (Sattolo),
1 thread, warm. The sweep was run **five times in five separate processes**; the figures
below are medians of the five pass medians. Same power state as above. Full tables:
`m3.2/analysis.md`; reasoning: `docs/notes/M3.2.md`.

| working set | ns/load | what it is |
|---|---:|---|
| 4 KiB | **1.02** | L1d — 4.9 cycles at the 4.83 GHz measured in M2.4 |
| 48 KiB | 1.10 | L1d, exactly at capacity |
| 64 KiB | **3.25** | the L1→L2 step, at the size `sysinfo` reports |
| 2 MiB | 6.07 | L2 (2 MiB per core), at capacity |
| 8 MiB | 46.01 | **bimodal**, 28–107 across passes — not a usable L3 figure |
| 24 MiB | 146.40 | DRAM |
| 1 GiB | **175.78** | DRAM |

The cleanest result in the project so far is the L1 step: latency triples between two
adjacent sweep points, at a cache size nothing told the benchmark about.

**There is no L3 plateau.** Between 4 MiB and 24 MiB — all of it inside a 24 MiB L3 —
latency climbs by a factor of ten with no flat region, and the 8 MiB point is bimodal: eight
separate runs gave 17.0, 17.2, 18.6, 19.3, 29.0, 35.9, 107.5, 129.9 ns. The *same vCPU id*
produced both clusters. Under WSL2 the guest cannot pin a thread to a physical core, and on
this hybrid part the core types do not share a path to the last-level cache. The
distribution is the result; there is no single L3 latency to quote.

**DRAM is 146–176 ns**, against PLAN.md's expected 90–110. These are the most repeatable
points in the sweep (pass-to-pass spread 1.0×), so the gap is not noise — it is the VM plus
a working set far larger than the flushed 256 KiB chase M2.3 measured 105 ns on. Latency
keeps rising after the caches are exhausted (+20 % from 24 MiB to 1 GiB) and that is *not*
the TLB: the huge-page grant is measured per buffer and every point ≥ 2 MiB got 100 %.

Turning huge pages off costs 7–12 % at ≥ 64 MiB. `--cold clflush` is 6.62× warm at 8 MiB,
because a cold lap of a 131072-line chase eats 16 ms of a 30 ms trial — on a chase, cold
mode is only interpretable when the trial completes many laps.

A preliminary variance note, *not* a Target #2 measurement: at 1 MiB over 200 trials,
`--cold clflush` gives CoV 33.97 % against warm's 8.59 %. Cold mode made repeatability
worse, which is the opposite of M3.3's stated hypothesis. Recorded for M3.3.

## Disk I/O — M4.1, 2026-09-21

Source: `m4.1/`. 4 GiB test file on ext4 — which is a **VHDX on the Windows host**, so these
are *virtual disk* numbers. Medians; reads 5 trials of 500 ms, sequential writes 3 trials of
300 ms. 1 MB = 1e6 bytes. Same power state as above.

| metric | 1 thread | 4 threads | 16 threads |
|---|---:|---:|---:|
| `disk_seq_read_bw` | 2690 MB/s | **6405 MB/s** | — |
| `disk_seq_write_bw` | 1487 MB/s | 1951 MB/s | — |
| `disk_rand_read_iops` | 9131 | 32808 | **89966** |
| `disk_rand_read_p99_us` | 274 µs | 234 µs | 300 µs |
| `disk_rand_write_iops` | 512 | 554 | 585 |

**Random writes barely scale** — 512 → 585 IOPS for sixteen times the threads — because
`O_DSYNC` makes every write durable before it returns and device flushes serialize. A
durable 4 KiB write costs about 2 ms here, **18× a 4 KiB read**. That is the number a
database's commit path lives on, and it is meant to look like this.

**The engine agrees with fio within ±5 %** — but only when the comparison is run
*alternating*. Run the obvious way (engine, then fio) the engine looked 12–18 % faster; run
alternating three times, every ratio is 0.958–1.030. The gap was drift in the host's cache
over the VHDX, which is M2.5's lesson in a new domain. fio 3.36 was built from source (not
packaged, no root).

**`O_DIRECT` is verified, not assumed.** `strace` confirms the flags, the 1 MiB / 4 KiB
transfer sizes, the 4096-aligned offsets and the 16:1 write-to-`fdatasync` ratio. Filling
the guest's page cache with the whole 4 GiB file changes the next run by **−0.1 %**, and the
first trial of a run is *slower* than the rest, not faster. What this cannot show is whether
Windows caches the VHDX; nothing run inside the guest can, which is why the label is
"virtual disk".

## Index

| directory | milestone | what it holds |
|---|---|---|
| `m1.2/` | M1.2 | measurement fundamentals: warmup effect, trial-length scaling, debug-vs-release |
| `m2.1/` | M2.1 | worker pool: start spread, pinned vs unpinned scaling, false sharing |
| `m2.2/` | M2.2 | per-configuration summaries and the `--max-seconds` safety cap |
| `m2.3/` | M2.3 | cold-cache infrastructure: flush vs evict, THP, cold-mode A/B |
| `m2.4/` | M2.4 | the three CPU kernels, the thread sweep, Target #1, frequency probe |
| `m2.5/` | M2.5 | the variance study: Target #2, the knob A/B, interleaving, the clock canary |
| `m3.1/` | M3.1 | memory bandwidth: the working-set sweep, the thread knee, write-allocate, `--nt` |
| `m3.2/` | M3.2 | cache latency: the pointer chase, the L1/L2 steps, huge pages, cold-vs-warm |
| `m4.1/` | M4.1 | disk I/O: O_DIRECT, the fio comparison, the cold check, `O_DSYNC` writes |

## Targets

| target | status | evidence |
|---|---|---|
| Engine ≥ 10M ops/s | **met** (M2.4) | `cpu_2026-09-20.json`, table above |
| CoV ≤ 3 % over ≥ 1000 trials | **not met** (CPU): best 3.47 % | `variance_2026-09-21.md`; M3.3 for memory |
| API ingests/queries ≥ 100K measurements | not yet run | M5.2 |
| API ≥ 1000 req/s, p95 ≤ 50 ms | not yet run | M5.4 |
| Dashboard: 12 metrics, 10K+ points | not yet run | M6.3 |

## Variance — M2.5, 2026-09-21

Target #2 (CoV ≤ 3 % over ≥ 1000 trials) is **not met on this machine**. Best achieved:
**3.47 %** (`cpu_fp_ops`, 1 thread, 50 ms trials, 1000 trials, no trimming). Full table,
conditions and noise diagnosis: `variance_2026-09-21.md`.

Two noise regimes. At 16–22 threads, thermal drift of 7.6–10.6 % across a 50-second
configuration plus host preemption — `cpu_fp` at 22 threads had a worker arrive more than
1 ms late off the start barrier in 91.8 % of trials. At 1 thread, no late trials and no
drift, and a distribution indistinguishable from Gaussian: the machine simply does not
repeat a 50 ms measurement better than ~3.5 %.

The target's two halves conflict on a laptop. Longer trials reduce CoV (200 ms reaches
2.83 % at 100 trials), but 1000 of them take 200 seconds per configuration, over which the
package heats — re-measured at the required 1000 trials, 200 ms gives 3.74–3.94 %.

The CoV column in `m2.4/README.md` is *not* the Target #2 measurement: those are 30-trial
runs made while sweeping thread counts.
