# M2.4 verification runs (2026-09-20)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`, never `-ffast-math`).

**Power state, recorded because it bounds every absolute number here:** plugged in
(`PowerLineStatus: Online`), Windows power plan **Balanced**, Windows power mode overlay
**Best power efficiency** (`961cc777-…`, set for both AC and DC), battery **low and
charging** (24 % at the start of the runs). Left as found rather than switched to
"Best performance", so every throughput figure below is a **floor**, not this machine's
ceiling. A laptop charging a low battery also shares the adapter's power budget with the
CPU package, which pushes the same way. Docker Desktop's two containers were stopped
before the measurement runs (`docker compose down`).

Interpretation is in `docs/notes/M2.4.md`. Defaults in force unless a command says
otherwise: `--warmup 5 --warmup-ms 500 --spin-ms 500 --cold clflush --trial-ms 50`.

| file | what it is / command (from the repo root) |
|---|---|
| `environment.txt` | power state, load average and container state at the start of the runs |
| `cpu_sweep_unpinned.json` | `bench run -w cpu_int,cpu_fp,cpu_hash -t 1,2,4,8,11,16,22 -n 30 --trial-ms 50` |
| `cpu_sweep_pinned.json` | same, plus `--pin` |
| `scaling_table.md` | the tables below, generated from those two files by `make_table.py` |
| `make_table.py` | `python3 make_table.py cpu_sweep_unpinned.json cpu_sweep_pinned.json` |
| `disasm_kernels.log` | hot-loop disassembly of all three kernels with exact expected instruction counts |
| `trial_length_scaling.log` | the M1.2 iteration-scaling check applied to all three kernels |
| `frequency_probe.log`, `freq_probe.cpp` | effective core frequency, measured (see below) |
| `hash_fold.log` | the A/B study behind `cpu_hash`'s accumulator fold |
| `ctest_presets.log` | 97/97 tests in release, debug, ci, asan and tsan |
| `cli_and_schema.log` | `bench list`, `bench validate`, `check-jsonschema` |

`../cpu_2026-09-20.json` is a copy of `cpu_sweep_unpinned.json` under the name
`PLAN.md` asks for (`docs/results/cpu_<date>.json`). The summary table built from it is in
`../README.md`.

## Why the frequency here is measured, not read

Every `ops/cycle` figure needs a clock, and nothing inside WSL2 reports a usable one:

- `/proc/cpuinfo` gives `cpu MHz : 3071.998` — the same value on all 22 vCPUs, which is
  impossible on a hybrid part with turbo. It is a Hyper-V synthetic number.
- `/sys/devices/system/cpu/cpu0/cpufreq` does not exist under WSL2.
- Windows reports `MaxClockSpeed 2300`, `CurrentClockSpeed 2300` — the nominal base clock,
  not a live reading.

So `freq_probe.cpp` measures it: time a chain of instructions that must execute one after
another, where the latency in cycles is known. Two instructions with *different* known
latencies are used, because one chain cannot tell you whether you measured the clock or
measured the CPU optimising your chain away:

```
dependent add reg,reg  max 4.363 G/s   => 4.363 GHz (latency 1)
dependent imul reg,reg max 1.400 G/s   => 4.200 GHz (latency 3)
dependent add $1,reg   max 25.577 G/s  <- NOT a clock
```

The first two agree to 3.9 %, so both chains were real chains: **~4.28 GHz** is used for
every `ops/cycle` number here, with roughly ±4 % of slack in it.

The third row is the trap. The first version of this probe used `add $1, %rax` and
reported 25 GHz. Recent Intel cores collapse chains of add-with-immediate to the same
register in the renamer, so that "dependency chain" was not one. Any single-chain
frequency probe would have published 25 GHz without noticing.

## Target #1: aggregate `cpu_*_ops` ≥ 10M ops/s at the best thread count

Unpinned run. PASS for all three, by two to three orders of magnitude.

| metric | best threads | aggregate ops/s | ≥ 1e7? | ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 16 | 3.711e+10 | **PASS** | 3.862e+09 | 0.90 |
| `cpu_fp_ops` | 22 | 8.587e+10 | **PASS** | 7.908e+09 | 1.85 |
| `cpu_hash_ops` | 16 | 1.083e+09 | **PASS** | 1.280e+08 | 0.03 |

The interesting column is the last one, and all three of its values were predicted from
the disassembly before they were measured:

| kernel | predicted from the hot loop | predicted ops/cycle | measured |
|---|---|---:|---:|
| `cpu_int` | 8 `imul` per iteration, 8 ops per iteration, one integer multiply port | 1.00 | 0.90 |
| `cpu_fp` | 8 lanes ÷ 4-cycle FMA latency | 2.00 | 1.85 |
| `cpu_hash` | 31 `imul` per block on one multiply port → ≥ 31 cycles/block | ≤ 0.032 | 0.030 |

All three land 7–8 % below the port-limit prediction, consistently. That is what you
expect if the sustained clock during a 50 ms benchmark trial is a few per cent below the
4.28 GHz the probe caught at its best.

## Scaling across thread counts

Aggregate throughput, median of 30 trials. "Scaling eff." is
`ops at N threads / (N × ops at 1 thread)`.

### unpinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV |
|---|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 3.862e+09 | 3.862e+09 | 100% | 15.0% |
| `cpu_int_ops` | 2 | 8.072e+09 | 4.036e+09 | 104% | 8.7% |
| `cpu_int_ops` | 4 | 1.660e+10 | 4.151e+09 | 107% | 5.4% |
| `cpu_int_ops` | 8 | 2.821e+10 | 3.526e+09 | 91% | 5.7% |
| `cpu_int_ops` | 11 | 3.413e+10 | 3.103e+09 | 80% | 4.8% |
| `cpu_int_ops` | 16 | **3.711e+10** | 2.320e+09 | 60% | 5.1% |
| `cpu_int_ops` | 22 | 3.299e+10 | 1.499e+09 | 39% | 10.9% |
| `cpu_fp_ops` | 1 | 7.908e+09 | 7.908e+09 | 100% | 8.6% |
| `cpu_fp_ops` | 2 | 1.574e+10 | 7.869e+09 | 100% | 11.2% |
| `cpu_fp_ops` | 4 | 3.276e+10 | 8.190e+09 | 104% | 5.8% |
| `cpu_fp_ops` | 8 | 5.625e+10 | 7.032e+09 | 89% | 4.0% |
| `cpu_fp_ops` | 11 | 6.744e+10 | 6.131e+09 | 78% | 3.9% |
| `cpu_fp_ops` | 16 | 8.501e+10 | 5.313e+09 | 67% | 3.2% |
| `cpu_fp_ops` | 22 | **8.587e+10** | 3.903e+09 | 49% | 7.1% |
| `cpu_hash_ops` | 1 | 1.280e+08 | 1.280e+08 | 100% | 12.3% |
| `cpu_hash_ops` | 2 | 2.634e+08 | 1.317e+08 | 103% | 8.5% |
| `cpu_hash_ops` | 4 | 5.457e+08 | 1.364e+08 | 107% | 5.7% |
| `cpu_hash_ops` | 8 | 8.629e+08 | 1.079e+08 | 84% | 4.7% |
| `cpu_hash_ops` | 11 | 9.960e+08 | 9.055e+07 | 71% | 3.9% |
| `cpu_hash_ops` | 16 | **1.083e+09** | 6.766e+07 | 53% | 6.9% |
| `cpu_hash_ops` | 22 | 1.064e+09 | 4.836e+07 | 38% | 7.1% |

### pinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV |
|---|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 3.970e+09 | 3.970e+09 | 100% | 12.3% |
| `cpu_int_ops` | 2 | 8.318e+09 | 4.159e+09 | 105% | 15.1% |
| `cpu_int_ops` | 4 | 1.687e+10 | 4.218e+09 | 106% | 5.6% |
| `cpu_int_ops` | 8 | 2.933e+10 | 3.666e+09 | 92% | 5.4% |
| `cpu_int_ops` | 11 | 3.345e+10 | 3.041e+09 | 77% | 3.6% |
| `cpu_int_ops` | 16 | 3.365e+10 | 2.103e+09 | 53% | 6.2% |
| `cpu_int_ops` | 22 | **3.398e+10** | 1.545e+09 | 39% | 7.2% |
| `cpu_fp_ops` | 1 | 7.524e+09 | 7.524e+09 | 100% | 10.2% |
| `cpu_fp_ops` | 2 | 1.569e+10 | 7.846e+09 | 104% | 10.2% |
| `cpu_fp_ops` | 4 | 3.244e+10 | 8.109e+09 | 108% | 10.5% |
| `cpu_fp_ops` | 8 | 5.294e+10 | 6.618e+09 | 88% | 6.9% |
| `cpu_fp_ops` | 11 | 6.229e+10 | 5.663e+09 | 75% | 4.4% |
| `cpu_fp_ops` | 16 | 7.669e+10 | 4.793e+09 | 64% | 4.4% |
| `cpu_fp_ops` | 22 | **8.099e+10** | 3.681e+09 | 49% | 6.7% |
| `cpu_hash_ops` | 1 | 1.150e+08 | 1.150e+08 | 100% | 11.0% |
| `cpu_hash_ops` | 2 | 2.455e+08 | 1.227e+08 | 107% | 7.7% |
| `cpu_hash_ops` | 4 | 4.994e+08 | 1.249e+08 | 109% | 5.3% |
| `cpu_hash_ops` | 8 | 8.260e+08 | 1.032e+08 | 90% | 4.4% |
| `cpu_hash_ops` | 11 | 1.041e+09 | 9.461e+07 | 82% | 3.7% |
| `cpu_hash_ops` | 16 | **1.101e+09** | 6.879e+07 | 60% | 4.0% |
| `cpu_hash_ops` | 22 | 1.078e+09 | 4.901e+07 | 43% | 9.4% |

Pinning did not help. At the best thread count the unpinned run is 9 % faster for
`cpu_int` and 6 % faster for `cpu_fp`; `cpu_hash` is a wash. This is the third milestone in
a row to find that (M2.1, M2.3), and the reason is in `CLAUDE.md`: pinning a thread to a
Hyper-V vCPU does not decide which physical core runs it, so the only thing guest-side
pinning removes is the Linux scheduler's ability to move a thread away from a busy vCPU.

## Trial-length scaling (the M1.2 check, applied to all three)

From `trial_length_scaling.log`. Double the trial length: the work must double and the
rate must not move.

| workload | ops @50 ms | ops @100 ms | ops ratio | ops/s @50 ms | ops/s @100 ms | rate ratio |
|---|---:|---:|---:|---:|---:|---:|
| `cpu_int` | 187,695,104 | 397,410,304 | 2.117 | 3.747e+09 | 3.966e+09 | 1.059 |
| `cpu_fp` | 373,293,056 | 749,731,840 | 2.008 | 7.450e+09 | 7.492e+09 | 1.006 |
| `cpu_hash` | 6,225,920 | 12,582,912 | 2.021 | 1.241e+08 | 1.253e+08 | 1.010 |

## Test suite

97/97 pass in **release, debug, ci, asan and tsan** (`ctest_presets.log`); 11/11 of the
`perf`-labelled subset and 86/86 with `-LE perf`. M2.4 added 16 tests.

Three timing-sensitive tests failed once each during an earlier back-to-back run of all
five presets — `Runner.TrialsAreTimeBoxedToWholeBatches` (ci),
`Timing.DoNotOptimizeKeepsLoopAlive` (asan) and `CpuScaling.CpuFpOpsScaleWithTrialLength`
(asan). All three then passed 5/5 in isolation on the same binaries, and the whole matrix
passed cleanly on the re-run recorded here. Two of the three predate M2.4. They are host
noise, recorded rather than hidden: M1.2's finding that the clock self-test is a
contention canary applies, and M2.5 is where this gets handled properly.
