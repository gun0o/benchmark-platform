# Methodology

How each metric is measured, and what its number does and does not mean. The normative
definition of every metric — what one "operation" or unit is — lives in `CLAUDE.md`; this
file records the measurement procedure behind it, the caveats that come with it, and the
places where a number is easy to misread.

It gains a section as each milestone lands. Sections present so far: **memory bandwidth**
(M3.1), **cache latency** (M3.2), **disk I/O** (M4.1). The milestone notes in `docs/notes/` carry the full
derivation and the measurements behind each claim; this file is the short version a reader
needs before quoting a number.

Everything here was measured on the machine described in `docs/results/README.md`: an Intel
Core Ultra 9 185H laptop (48 KiB L1d, 2 MiB L2 per core, 24 MiB L3, 16 GB) running Ubuntu
22.04.5 under WSL2, so every CPU count is a Hyper-V vCPU count. See "Environment caveats"
in `CLAUDE.md`.

## Rules that apply to every metric

- **Timing** is `std::chrono::steady_clock` only, from the instant the start barrier
  releases every worker to the instant the last worker finishes. Setup, allocation,
  first-touch and cold preparation are outside that window by construction.
- **Warmup trials are run and never emitted.** A configuration warms up for at least
  `--warmup` trials *and* at least `--warmup-ms` of its own trial time.
- **Statistics** are defined in `engine/include/bench/stats.hpp` and pinned by numpy
  fixtures: `stddev` is the sample standard deviation (n−1), percentiles use numpy's
  `linear` interpolation, `cov` = stddev ÷ mean, `mad` = median(|x − median(x)|).
- **No outlier trimming, ever.** Trials that ran are reported. Run-quality problems are
  recorded in their own fields (`late_trials`, `canary_attempts`, `clock_call_ns_*`) next
  to the value, never used to remove one.
- **What was asked for and what happened are separate fields.** `params.cold` records what
  cold preparation the engine actually performed, which is not always what `--cold`
  requested; `params.nt_effective` records whether `--nt` applied; `params.huge_pages`
  records what `/proc/self/smaps` says was granted, not what was requested.

---

## Memory bandwidth — `mem_read_bw`, `mem_write_bw`, `mem_copy_bw` (M3.1)

Full derivation and the measurements behind every claim: `docs/notes/M3.1.md`. Raw runs:
`docs/results/m3.1/`.

### Procedure

Each worker thread owns a private buffer of `working_set_bytes`, allocated and
first-touched on that thread, page-aligned, and backed by transparent huge pages when it is
at least 2 MiB (`--no-hugepages` turns that off). One configuration measures one of the
three metrics; a trial never mixes them, because each kernel leaves the cache in a state
the next would inherit.

- **Read** sums the buffer as `uint64_t` through eight independent accumulators. Eight,
  because one accumulator makes the loop a dependency chain and measures add latency
  instead of load bandwidth. GCC vectorizes it to two 256-bit `vpaddq` accumulators;
  the disassembly is committed at `docs/results/m3.1/disasm_mem_bw.log`.
- **Write** stores a value obtained from a runtime RNG over the whole buffer. The value is
  deliberately runtime-derived so the loop cannot become `memset`: `memset` is a legitimate
  way to measure write bandwidth, but it is a different primitive, and which one ran should
  not depend on the optimizer noticing a byte-uniform constant.
- **Copy** is `std::memcpy` from a source buffer to a destination buffer, each
  `working_set_bytes`, so a copy configuration allocates twice the working set.

`value = bytes ÷ elapsed ÷ 1e9`, with **1 GB = 1e9 bytes**.

A batch is a whole number of complete passes over the buffer, repeated until at least
4 MiB has moved (1024 passes at 4 KiB, exactly one at 256 MiB). A single 4 KiB pass takes
about 40 ns — roughly three clock reads — so timing one pass at a time would have been
measuring the clock. Each pass ends with a compiler barrier, without which the optimizer
may legally run one pass and reuse its result.

### Counting rules, and what they exclude

- **Copy counts destination bytes once, not read + written.** A 21 GB/s copy moves 21 GB of
  payload per second; the memory controller saw 42 GB of traffic. Some tools report the
  doubled number, so a copy figure is not comparable across tools without checking this.
- **Read and write count the bytes the span covers**, not the DRAM traffic they caused. A
  plain (non-`--nt`) write causes roughly twice its own byte count in traffic; see
  write-allocate below.

### Write-allocate: why a write costs more than it looks like it should

A store writes 8 bytes; a cache line is 64. A store to a line that is not already resident
therefore cannot just write — the line must be **fetched from memory first** and then
modified. Every byte written to a non-resident line costs a byte read as well.

This is the sharpest feature in the working-set sweep. At the moment the buffer stops
fitting in the 48 KiB L1d (64 KiB working set), measured single-thread write bandwidth
falls from **290.9 to 80.6 GB/s, a 3.6× cliff**, while read bandwidth over the same step
falls only 1.3×. Nothing changed in the store loop; what changed is that each store now
drags a line in behind it.

The common rule of thumb that follows — "write ≈ ½ read in the DRAM regime" — is directionally
right and numerically wrong here. Measured at a 256 MiB working set, write/read is **0.61
at 1 thread and 0.61 at 8 threads**, not 0.50. The mechanism is confirmed; the arithmetic
assumes the read figure is the memory system's ceiling, and at low thread counts it is not
(see the concurrency note below).

**`--nt` (non-temporal stores)** writes whole 32-byte chunks through write-combining
buffers straight to memory, skipping the fetch. It removes both the write-allocate traffic
and the store-side concurrency limit. Measured at 256 MiB: at 8 threads write/read goes
from 0.61 to **0.90** — "approaches read", as expected. At 1 thread it goes to **1.88**,
i.e. a single core can push 43.5 GB/s of writes while pulling only 23.1 GB/s of reads,
because reads there are concurrency-limited and NT writes are not.

### `mem_copy_bw` without `--nt` is not a plain-store baseline at large working sets

**glibc's `memcpy` switches to non-temporal stores by itself** above
`glibc.cpu.x86_non_temporal_threshold`, which defaults to the L3 size on this system
(`getconf LEVEL3_CACHE_SIZE` = 25165824 bytes). At a 256 MiB working set, a copy run
*without* `--nt` is already using NT stores.

Measured by forcing the threshold above the working set with `GLIBC_TUNABLES`: the 256 MiB
copy loses **12.2 %** when denied NT stores, while an 8 MiB copy — below the threshold
either way — does not move (+2.8 %, within noise).

Consequence when reading the results: at DRAM-sized working sets, `mem_copy_bw` and
`mem_write_bw` are **not on the same store path**, and the fact that copy's number exceeds
write's is not a paradox. Compare copy against copy.

### Single-thread bandwidth is a property of the core, not of the memory

One core can only keep so many cache misses in flight at once (its line-fill buffers), so
single-thread read bandwidth is bounded by (outstanding misses × 64 bytes) ÷ memory
latency, whatever the DIMMs can do. Measured here: 23.1 GB/s at 1 thread rising to
**84.2 GB/s at 16 threads** on the same 256 MiB-per-thread working set, with the knee at
11–16 threads. A "single-thread memory bandwidth" figure is a statement about the core.

### Reading the working-set sweep

The sweep is a picture of the cache hierarchy. Two things in it are routinely misread:

- **A working set comfortably below the L3 size is not necessarily served by L3.** At
  16 MiB of a 24 MiB L3, reads run at 45 GB/s against L3's 72 and DRAM's 23, with the
  sweep's worst CoV. L3 is shared with everything else on the machine, including — under
  WSL2 — the host.
- **The copy curve is the read/write curve shifted one step earlier**, because a copy
  configuration allocates twice the working set and therefore leaves each cache level at
  half the nominal size.

### Cold mode has little effect on a bandwidth metric, by construction

`--cold clflush` flushes the buffer before every trial when it could have been cache
resident, and records `params.cold = "n/a"` when the working set exceeds L3 (flushing
512 MiB costs ~100 ms and cools nothing that was warm). But a 50 ms trial over a 1 MiB
buffer makes thousands of passes and only the first is cold, so the effect on the reported
bandwidth is well under a percent. This is the difference between a throughput metric,
where cache state is a transient, and a latency metric, where it is the whole measurement.

### What these numbers are not

They are **virtual-machine numbers**. The 22 "CPUs" are Hyper-V vCPUs, the hybrid P/E-core
layout is hidden from the guest, and pinning a thread to a vCPU does not determine which
physical core runs it. The shape of the curves is trustworthy; the absolute peak is a lower
bound on what the hardware can do.

A configuration using all 22 vCPUs leaves none for the coordinating thread or the host:
`mem_read_bw` at 22 threads reports 64 GB/s against 16 threads' 84 GB/s, with a worker
arriving more than 1 ms late off the start barrier in **100 %** of trials. That row is
published with its `late_trials` count rather than dropped, but it is not a fact about
memory bandwidth.

---

## Cache latency — `mem_latency` (M3.2)

Full derivation and the measurements behind every claim: `docs/notes/M3.2.md`. Raw runs:
`docs/results/m3.2/`.

### Procedure

Each worker owns a private buffer of `working_set_bytes` seen as an array of 64-byte lines.
A random **cyclic** permutation of the lines is built with **Sattolo's algorithm**, and each
line's first 8 bytes hold the index of the next line. The kernel is `idx = words[idx]`,
repeated: one op is one dependent load, and `value = elapsed ÷ loads`.

Sattolo, not Fisher–Yates, and the distinction is the whole measurement. Fisher–Yates
produces a uniformly random permutation, which decomposes into about ln(n) disjoint cycles
(measured: 8.8 on average for 4096 lines). A chase started anywhere would fall into one
cycle and stay there, so a "256 KiB working set" could report the latency of whatever cache
a 30 KiB subset fits in. Sattolo produces a uniformly random *cyclic* permutation, so the
chase visits every line exactly once per lap by construction.

A batch is 65536 loads. That is smaller than PLAN.md's 1 Mi: the trial loop can only
overshoot a trial's budget, never cut a batch short, and at ~160 ns per DRAM load a 1 Mi
batch would take 160 ms against a 50 ms trial.

Buffers of at least 2 MiB request transparent huge pages. Whether the kernel granted them is
**measured**, per buffer, and reported as `params.buffer_huge_page_bytes`; the active THP
policy is reported as `params.thp_policy`. Both matter, because a large chase over 4 KiB
pages is partly a TLB benchmark.

### A latency is per thread

`--threads N` measures **loaded latency**: the time one dependent load takes while N threads
are hitting the memory system. The value divides wall time by the loads **one** thread
performed, not by the sum across threads. Summing would report latency ÷ N — a figure that
improves as you add threads, which is the opposite of what loaded latency does. Rates are
summed across threads; latencies are not.

### Reading the sweep on this machine

| working set | ns/load | note |
|---|---:|---|
| 4 KiB – 48 KiB | 1.02 – 1.10 | L1d (48 KiB). 1.02 ns is 4.9 cycles at the measured 4.83 GHz. |
| 64 KiB – 2 MiB | 3.25 – 6.07 | L2 (2 MiB per core); rises towards capacity rather than being flat |
| 4 – 24 MiB | 15.3 – 146 | **no L3 plateau**; see below |
| 32 MiB – 1 GiB | 155 – 176 | DRAM |

Three caveats carry with any of these numbers:

- **There is no usable L3 figure for this machine.** Between 4 MiB and 24 MiB — all of it
  inside a 24 MiB L3 — latency climbs by a factor of ten with no flat region. The 8 MiB
  point is **bimodal**: eight separate runs gave 17.0, 17.2, 18.6, 19.3, 29.0, 35.9, 107.5,
  129.9 ns, two clusters rather than one spread. The same vCPU produced both, so it is not
  the vCPU. Under WSL2 the guest cannot pin a thread to a physical core, and on a hybrid
  part the core types do not share a path to the last-level cache. What is measurable here
  is the distribution, not a latency.
- **DRAM latency keeps rising after the caches are exhausted**, from 146 ns at 24 MiB to
  176 ns at 1 GiB. This is not the TLB: every point from 2 MiB up got 100 % of its buffer in
  huge pages, and 1 GiB is 512 huge pages. It is DRAM row and bank locality — a random
  pattern over a wider address span hits an open row less often.
- **PLAN.md expected 90–110 ns for DRAM; the measured figure is 146–176 ns.** These are the
  most repeatable points in the sweep (pass-to-pass spread 1.0×), so the gap is not noise.
  M2.3's 105 ns came from a flushed 256 KiB chase, which is an easier case than a
  steady-state chase over a large working set, and its run-to-run range was 49–217 ns.

### `--no-hugepages` costs 7–12 % at large working sets

Measured over three alternating pairs: +7 % at 64 MiB, +12 % at 256 MiB, +10 % at 1 GiB.
Real and consistent in sign, smaller than one might expect — a page walk is four memory
accesses that are themselves cached, so against a ~195 ns DRAM access it is a tax rather
than a doubling.

### `--cold` on a chase is only interpretable when the trial completes many laps

Unlike a bandwidth kernel, where the cold cache state is a transient in the first of
thousands of passes, a chase can spend an entire trial on its first lap.

| working set | warm | `--cold clflush` | ratio |
|---|---:|---:|---:|
| 256 KiB | 3.28 | 3.27 | 1.00× |
| 1 MiB | 4.42 | 4.69 | 1.06× |
| 8 MiB | 17.79 | 117.88 | **6.62×** |
| 64 MiB | 156.42 | 156.29 | 1.00× (`params.cold` = `n/a`) |

At 8 MiB the chase has 131072 lines and a cold lap costs about 16 ms of a 30 ms trial, so
`--cold clflush` there does not measure "L3 latency from a cold start" — it measures
cold-miss latency. Above L3 the engine records `params.cold = "n/a"` and does not flush at
all, so warm and cold agree to 0.1 %.

**Preliminary variance note**, not a Target #2 measurement (200 trials, not ≥ 1000; the full
study is M3.3): at a 1 MiB working set, `--cold clflush` gives CoV **33.97 %** against warm's
**8.59 %** — cold mode made repeatability *worse*, not better as PLAN.md's M3.3 hypothesis
expects. The distribution says why: MAD/median is 5.2 %, and 4 trials of 200 are over twice
the median. It is a heavy tail from the flush's own work and the cold first lap, not a wider
spread.

### Envelope timestamps are not measurements

`started_at` and `finished_at` come from `system_clock`, which WSL2 resynchronizes against
the Windows host and which can jump in either direction. One run during M3.2 reported a
5 minute 47 second span for 1.7 seconds of work, and the next run's timestamps were earlier
than that "finish". Every measurement uses `steady_clock`; `duration_ns` per trial is the
figure to trust. Do not compute a throughput from `finished_at − started_at`.

---

## Disk I/O — `disk_seq_*`, `disk_rand_*` (M4.1)

Full derivation and the measurements behind every claim: `docs/notes/M4.1.md`. Raw runs:
`docs/results/m4.1/`.

### These are "virtual disk" numbers

The filesystem under the test file is ext4 on `/dev/sde`, which is a **VHDX file on the
Windows host**, not a physical device. `O_DIRECT` bypasses the **guest's** page cache — and
that is measured, not assumed (see the cold check below) — but it says nothing about whether
Windows is caching the VHDX. The guest cannot see, drop or measure the host's cache;
`drop_caches` needs root and would affect only the guest anyway.

So every figure here is what a program running in this WSL2 guest sees. That is a useful
thing to know. It is not the device's number, and it is labelled accordingly everywhere.

### Procedure

A pre-created test file (`--disk-path`, default `$HOME/.cache/bench/testfile`, default size
4 GiB) is **filled with random data** through 1 MiB `O_DIRECT` writes plus `fdatasync`, never
left sparse and never zeroed: a sparse file's reads never reach the device, and
thin-provisioned, compressing or deduplicating layers short-circuit runs of zeros. Buffers
come from `posix_memalign(4096)`, which satisfies `O_DIRECT`'s alignment rule for both the
512-byte logical and the 4096-byte physical sector size of this device.

Before every trial, outside the timed region, the engine issues
`posix_fadvise(POSIX_FADV_DONTNEED)` over the whole file — belt and braces alongside
`O_DIRECT`, since something else may have read the file through the page cache.

- **`disk_seq_read_bw` / `disk_seq_write_bw`**: 1 MiB `pread`/`pwrite`. Each thread owns a
  contiguous block-aligned region of the file and wraps inside it, so N threads are N
  sequential streams rather than N threads interleaving into one. A batch is 16 blocks; for
  writes, `fdatasync` is called at the end of every batch, **inside the timed region**, so
  the reported throughput includes durability. (PLAN.md says "at trial end"; a workload
  cannot see a trial boundary, and per batch is stricter.)
- **`disk_rand_read_iops` / `disk_rand_write_iops`**: 4 KiB `pread`/`pwrite` at uniformly
  random 4 KiB-aligned offsets over the whole file, `std::mt19937_64` seeded per thread.
  Queue depth is the thread count: one outstanding synchronous I/O per thread. Random writes
  add `O_DSYNC`, because the metric is defined as **durable** 4 KiB writes.
- **`disk_rand_read_p99_us`**: every read is timed with `steady_clock` into a 1 µs-bucket
  histogram; the runner merges the threads' histograms after the trial's end barrier and
  takes the 99th percentile. It is emitted from the **same trial** that produced
  `disk_rand_read_iops`, not from a separate run, so the two describe the same reads.

`--trial-ms` defaults to **500** when any selected metric is a disk metric (50 otherwise),
because a 4 KiB read is ~110 µs and a 50 ms trial would hold too few samples for a p99.

### The p99 is nearest-rank, unlike every other percentile in this project

`stats.hpp` pins percentiles to numpy's `linear` interpolation and trial-level summaries
still use it. The p99 does not: interpolating between two adjacent 1 µs buckets would invent
precision the histogram does not have, since the samples were rounded to a microsecond on the
way in. It is the smallest bucket whose cumulative count reaches `ceil(0.99 n)`, reported at
that bucket's **upper edge**, so it answers "99 % of reads completed within X µs". Quantized
by at most 1 µs, which is under 0.4 % of the 274 µs measured here.

### Write metrics are gated, twice

`--allow-writes` is required for `disk_seq_write_bw` and `disk_rand_write_iops` at all, and
`--write-budget` (default 1 GiB) stops the run at a trial boundary once that many bytes have
been written — the same mechanism as `--max-seconds`, so every trial that ran is a whole
trial. A run that raises the budget records the raised value in its `argv`.

### Measured on this machine

| metric | 1 thread | 4 threads | 16 threads |
|---|---:|---:|---:|
| `disk_seq_read_bw` | 2690 MB/s | 6405 MB/s | — |
| `disk_seq_write_bw` | 1487 MB/s | 1951 MB/s | — |
| `disk_rand_read_iops` | 9131 | 32808 | 89966 |
| `disk_rand_read_p99_us` | 274 µs | 234 µs | 300 µs |
| `disk_rand_write_iops` | 512 | 554 | 585 |

1 MB = 1e6 bytes. Two things are easy to misread:

- **Random writes barely scale**: 512 → 585 IOPS for sixteen times the threads. `O_DSYNC`
  makes each write durable before it returns, and device-level flushes serialize however many
  threads ask for one. A durable 4 KiB write costs about 2 ms here, **18×** a 4 KiB read.
  This is the number a database's commit path lives on, and it is meant to look like this.
- **The p99 is far from the mean even on an idle device**: at QD 1, 274 µs against a 110 µs
  mean. That gap is the reason the metric exists next to IOPS.

### Agreement with fio, and how it has to be measured

fio (`--direct=1 --ioengine=psync`, same block sizes, same job counts) is the reference.
Run the obvious way — engine, then fio — the engine looked **12–18 % faster**. Run
**alternating**, three repetitions, every ratio is within **±5 %**: 0.958–1.030 across
sequential and random reads at every thread count.

The gap was drift, not a tool difference, and the drifting variable is the host's cache over
the VHDX. This is M2.5's lesson in a different domain: on a machine whose state changes, a
single A-then-B comparison measures the order as much as the thing. For scale, fio's own
QD 1 answer moves between 7763 and 8815 IOPS depending only on which of four reasonable
parameter choices it is given.

### The cold check, and what it does and does not prove

Ten 300 ms trials with no warmup, then the whole 4 GiB file read into the guest page cache
(`buff/cache` grew from 4251 MB to 8347 MB), then ten more:

| | median | trial 0 / the rest |
|---|---:|---:|
| cold start | 2377 MB/s | 0.878 |
| guest page cache deliberately filled | 2375 MB/s | 0.896 |

**−0.1 %.** Four gigabytes in the guest's page cache are worth nothing to the next read, so
`O_DIRECT` is doing what it claims. Trial 0 is *slower* than the trials after it in both runs
— the opposite of a caching effect, and consistent with M1.2's first-trial cost.

This does not rule out host-side caching, and nothing run from inside the guest can. The
2.7 GB/s single-thread figure is within what a PCIe 4 NVMe does unaided, so it is not
evidence either way.
