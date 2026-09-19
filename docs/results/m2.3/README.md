# M2.3 verification runs (2026-09-19)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`). L1d 48 KiB/core,
L2 2 MiB/core, L3 24 MiB total. Interpretation is in `docs/notes/M2.3.md`.

**Power state during these runs** — recorded because it changes the numbers:

| | |
|---|---|
| AC adapter | **plugged in** (`/sys/class/power_supply/AC1/online = 1`, battery charging, 94 %) |
| Windows power plan | Balanced (`381b4222-f694-41f0-9685-ff5bb260df2e`) |
| Windows power mode | **Best power efficiency** (`961cc777-2547-4f9d-8174-7d86181b8a7a`), on both AC and DC |
| Load average (before / after) | 1.02 / 2.00 |
| THP policy | `always [madvise] never` |

"Best power efficiency" is the least favourable of the three Windows power modes for
benchmarking: it holds clocks down and biases the scheduler toward E-cores. It was the mode
already set on the machine and was deliberately left alone, so these are floor numbers, not
best-case ones. M2.1 and M2.2 record "plugged in" but not which power mode was active, so
absolute values here are not strictly comparable to theirs. Every ratio this milestone
verifies is measured within a single run, where the power state is constant, so the ratios
are unaffected.

Full capture: `environment.txt`.

## Files

| file | command (from `engine/`) |
|---|---|
| `cold_cache_test.log` | `./build/release/tests/cold_cache_test` — Verify 1, 2, 3 and the control |
| `cold_mechanism_stability.log` | the two ratio tests, ten consecutive runs |
| `cache_test.log` | `./build/release/tests/cache_test` — 16 functional tests, incl. the THP table |
| `thp_alignment.log` | counterfactual: does 2 MiB alignment / `MADV_HUGEPAGE` matter? |
| `flush_cost.log` | where the cost of `flush_lines` goes: instructions, fence, per-line rate |
| `ctest_all_presets.log` | `ctest --preset {release,debug,asan,tsan,ci} -LE perf`, then `-L perf` |
| `ci_portable_clflushopt.log` | CLFLUSHOPT reached at runtime from a build whose `-march` lacks it |
| `schema_validation.log` | all three modes through `check-jsonschema` and `bench validate`, plus a negative |
| `cold_clflush.json` | `bench run -w cpu_int -t 1,8 -n 50 --trial-ms 50 --pin --cold clflush` |
| `cold_evict.json` | same, `--cold evict` |
| `cold_none.json` | same, `--cold none` |
| `verbose_sample.log` | `bench run -w cpu_int -t 4 -n 4 --trial-ms 30 --pin --cold clflush --verbose` |

## Verify 1 — a flush makes the next access miss to DRAM

256 KiB random cyclic pointer chase (Sattolo), warm (L2-resident) against the first pass
after `flush_lines`. 41 repetitions. Requirement: ≥ 5×.

```
clflush 256 KiB  n=41  warm 3.65  cold p5 100.03 / med 104.83 / p95 121.93 ns/load
                       ratio 28.74x  (per-rep 15.62x..34.96x)   require >= 5.0x
```

3.65 ns/load is L2 latency; 105 ns/load is DRAM. **28.7×**, the target exceeded 5.7×.

## Verify 2 — eviction works too

Same chase, cooled by streaming a private 48 MiB buffer (2 × L3) instead of flushing.
Requirement: ≥ 3×.

```
evict 48 MiB     n=41  warm 3.73  cold p5 38.33 / med 57.51 / p95 117.97 ns/load
                       ratio 15.41x  (per-rep 6.44x..53.54x)   require >= 3.0x
```

**15.4×**, the target exceeded 5.1×.

Ten consecutive runs of both (`cold_mechanism_stability.log`), which is what says how far
either number can be trusted:

| run | clflush warm | clflush cold med | clflush ratio | evict warm | evict cold med | evict ratio |
|---|---|---|---|---|---|---|
| 1 | 3.65 | 109.52 | 29.97× | 6.98 | 112.49 | 16.12× |
| 2 | 4.59 | 109.55 | 23.89× | 3.74 | 96.29 | 25.78× |
| 3 | 3.65 | 64.62 | 17.71× | 4.86 | 45.71 | 9.41× |
| 4 | 3.65 | 49.36 | 13.52× | 5.35 | 95.94 | 17.93× |
| 5 | 3.65 | 106.89 | 29.27× | 3.73 | 56.29 | 15.08× |
| 6 | 3.65 | 107.57 | 29.47× | 5.35 | 87.35 | 16.33× |
| 7 | 6.98 | 217.45 | 31.14× | 5.35 | 88.30 | 16.51× |
| 8 | 3.65 | 105.54 | 28.93× | 5.95 | 101.53 | 17.08× |
| 9 | 3.65 | 103.82 | 28.47× | 4.34 | 80.90 | 18.64× |
| 10 | 5.02 | 110.17 | 21.96× | 4.33 | 57.06 | 13.17× |

| | min | median | max |
|---|---|---|---|
| clflush ratio | 13.52× | 28.70× | 31.14× |
| clflush cold median | 49.4 ns | 107.2 ns | 217.4 ns |
| evict ratio | 9.41× | 16.42× | 25.78× |
| evict cold median | 45.7 ns | 87.8 ns | 112.5 ns |

Both mechanisms clear the region in every one of ten runs, with the worst case 2.7× above
its bar (clflush) and 3.1× above it (evict). The flush cools more thoroughly — its cold
median is higher in 8 of 10 runs — which is one reason it is the default; the other is that
it costs 0.9 µs instead of 2.4 ms (see below).

The absolute latencies move by 2–4× between runs (clflush cold median 49–217 ns) while the
*ordering* never changes. That spread is the machine, not the mechanism: the measuring
thread lands on a P-core, an E-core or an LP E-core from run to run, and those have
different L2 sizes and different distances to memory. It is the hybrid-topology caveat from
`CLAUDE.md` showing up in a direct measurement.

**On sample size.** An earlier version of this test used 9 repetitions and reported ratios
between 2.95× and 23.63× on the same binary and the same machine, failing its own 3× bar
once. The sample size was the defect, not the eviction. 41 repetitions is what makes the
median mean something.

## Verify 3 — cold costs the first pass and nothing afterwards

PLAN.md specifies this on `mem_latency`, which is M3.2. It runs here on the same pointer
chase that workload will be built from. ns per dependent load, median of 10 trials after 2
discarded; p1..p5 are the five passes following `flush_lines`:

| working set | warm | p1 | p2 | p3 | p4 | p5 |
|---|---|---|---|---|---|---|
| 64 KiB | 3.63 | **130.56** | 3.24 | 3.63 | 3.63 | 3.63 |
| 256 KiB | 3.82 | **106.03** | 3.84 | 3.82 | 3.82 | 3.82 |
| 1024 KiB | 6.47 | **143.92** | 7.53 | 6.84 | 6.78 | 6.82 |

First pass 22–36× the warm value; by pass 2–3 it is back to the warm value. Cold mode sets
the starting state and then stops mattering, which is exactly the requirement — a cold mode
that still cost something on pass 5 would be changing the measurement rather than
controlling its starting point.

Control (`--cold none`): warm 5.35, "cold" 5.35, ratio **1.00×**. Without this row the two
tests above would also pass for a chase that was simply slow every time.

## Cold preparation cost, per trial, per thread

From the three engine runs. Preparation happens before the start barrier, so none of this is
inside a timed region; "overhead" is wall-clock time added to a 50 ms trial.

| cold | threads | bytes/thread | mean µs | median µs | p95 µs | max µs | overhead |
|---|---|---|---|---|---|---|---|
| clflush | 1 | 80 | 0.88 | 0.85 | 1.51 | 1.8 | 0.00 % |
| clflush | 8 | 80 | 0.79 | 0.74 | 1.30 | 13.4 | 0.00 % |
| evict | 1 | 50 331 648 | 3128.29 | 2357.33 | 7987.70 | 10695.1 | 4.71 % |
| evict | 8 | 50 331 648 | 5887.23 | 5741.82 | 6763.43 | 16106.8 | 11.48 % |
| none | 1 | 0 | 0.15 | 0.15 | 0.24 | 0.3 | 0.00 % |
| none | 8 | 0 | 0.15 | 0.12 | 0.30 | 0.7 | 0.00 % |

`none` at 0.15 µs is the measurement floor: two `steady_clock` reads around an empty
operation. `evict` at 8 threads streams 8 × 48 MiB = 384 MiB per trial through one shared
memory system, which is why it costs 2.4× what it does at 1 thread.

Where does `clflush`'s 0.85 µs go, for a region of only 80 bytes? Measured separately
(`flush_cost.log`, median of 1800 calls; the two clock reads cost 15 ns and are in every
row):

| | cost |
|---|---|
| two clock reads, nothing between them | 15 ns |
| two `CLFLUSHOPT`s, no fence | 15 ns |
| + `SFENCE` | 17 ns |
| + `MFENCE` (what `flush_lines` does) | 128–422 ns across runs |
| `MFENCE` alone | 27 ns |

The flush instructions themselves are free — indistinguishable from the clock overhead. The
cost is the `MFENCE` *waiting for the writeback to reach memory*, which is a DRAM round trip
and is why it is far more than a bare `MFENCE`. This micro-measurement is itself noisy
(~100 ns resolved with a 15 ns clock inside a VM), so it is quoted as a range.

Marginal cost per line, which is what decides the `n/a` rule:

| region | clean lines | dirty lines |
|---|---|---|
| 80 B | 64 ns/line | 147 ns/line |
| 4 KiB | 4.06 ns/line | 17.73 ns/line |
| 64 KiB | 2.61 ns/line | 12.92 ns/line |
| 256 KiB | 2.53 ns/line | 5.93 ns/line |
| 1 MiB | 1.19 ns/line | 5.52 ns/line |

A workload's buffer is dirty when it is flushed, so ~5.5 ns/line is the rate that matters:
1 MiB costs 90 µs, and 512 MiB would cost roughly 46 ms. PLAN.md's estimate of "~100 ms per
512 MiB" is the right order of magnitude, and it is why a region larger than the LLC is
recorded as `n/a` instead of being flushed.

## Cold mode does not change the `cpu_int` measurement

50 trials × 50 ms, pinned, per configuration:

| cold | threads | n | median Gops/s | CoV | `params.cold_bytes` | `params.huge_pages` |
|---|---|---|---|---|---|---|
| clflush | 1 | 50 | 3.801 | 20.13 % | 80 | false |
| evict | 1 | 50 | 3.731 | 16.67 % | 50 331 648 | true |
| none | 1 | 50 | 3.807 | 16.62 % | 0 | false |
| clflush | 8 | 50 | 30.461 | 6.33 % | 80 | false |
| evict | 8 | 50 | 30.704 | 5.55 % | 50 331 648 | true |
| none | 8 | 50 | 30.112 | 8.97 % | 0 | false |

The three modes differ by under 2 %, far inside a CoV of 6–20 %. That is the expected and
correct result: `cpu_int`'s entire state is 80 bytes, so cooling it costs a handful of cache
misses at the start of a 50 ms trial. The run's real value is as evidence that cold
preparation stays *outside* the timed region — if any of it leaked in, the `evict` rows
would be about 10 % slower, since `evict` adds 5.7 ms of work to every 50 ms trial.

The CoV column is not a variance result. That is M2.5 (1000 trials, cold mode on); these are
50 trials in the least favourable Windows power mode.

## Transparent huge pages

From `cache_test.log`:

```
  2 MiB requested ->   2 MiB in huge pages (100.0%)
  8 MiB requested ->   8 MiB in huge pages (100.0%)
 48 MiB requested ->  48 MiB in huge pages (100.0%)
128 MiB requested -> 128 MiB in huge pages (100.0%)
```

And the counterfactual (`thp_alignment.log`), 48 MiB anonymous mapping:

| mapping | AnonHugePages granted |
|---|---|
| 2 MiB-aligned + `MADV_HUGEPAGE` | 48 MiB of 48 |
| 2 MiB-aligned, no madvise | 0 MiB |
| unaligned + `MADV_HUGEPAGE` | 46 MiB of 48 |
| unaligned, no madvise | 0 MiB |

Under the `madvise` policy the `madvise` call is what does the work; the 2 MiB alignment
buys back the partial huge page at each end. Both are done, and the grant is read back from
`/proc/self/smaps` rather than assumed.

## Test suites

71 functional tests pass in the release, debug, asan, tsan and **ci** (portable
`-march=x86-64-v3`) presets, and all 7 perf-labelled tests pass in release:

```
release  71/71   3.29 s        asan  71/71   5.48 s        ci    71/71   3.27 s
debug    71/71   3.48 s        tsan  71/71   5.08 s        perf   7/7    4.76 s
```

16 of the 71 are new (`cache_test`), plus 4 new perf tests (`cold_cache_test`).

The `ci` preset is compiled for `x86-64-v3`, which does not include CLFLUSHOPT, and still
contains the instruction (14 sites) and executes it through the runtime CPUID check —
`ci_portable_clflushopt.log`:

```
$ ./build/ci/tests/cold_cache_test --gtest_filter='*Flush*'
clflush 256 KiB  n=41  warm 3.65  cold med 106.19 ns/load  ratio 29.09x
```

## Schema

All three cold modes round-trip through `check-jsonschema` against
`schema/benchmark-result.schema.json` and through `bench validate` (100 results each). A
result with `params.cold` outside the enum is rejected by both — `schema_validation.log`:

```
bench validate  : results[0].params.cold: unknown 'maybe'
check-jsonschema: $.results[0].params.cold: 'maybe' is not one of ['clflush','evict','none','n/a']
```
