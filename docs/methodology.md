# Methodology

How each metric is measured, and what its number does and does not mean. The normative
definition of every metric — what one "operation" or unit is — lives in `CLAUDE.md`; this
file records the measurement procedure behind it, the caveats that come with it, and the
places where a number is easy to misread.

It gains a section as each milestone lands. Sections present so far: **memory bandwidth**
(M3.1). The milestone notes in `docs/notes/` carry the full derivation and the measurements
behind each claim; this file is the short version a reader needs before quoting a number.

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
