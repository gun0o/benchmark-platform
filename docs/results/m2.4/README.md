# M2.4 verification runs (2026-09-20)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`, never `-ffast-math`).

**The headline numbers here were measured twice, under two Windows power modes**, because
the first set was taken under "Best power efficiency" and that turned out to cost 8–17 %.
The Best performance run is the one quoted everywhere else; both are kept, because the
difference between them is itself a measurement (see "Power mode A/B" below).

| | first run | headline run |
|---|---|---|
| Windows power plan | Balanced | Balanced |
| Power mode overlay | Best power efficiency (`961cc777…`) | **Best performance** (`ded574b5…`, "Max Performance Overlay") |
| Power | plugged in, battery low and charging (24 %) | plugged in, battery charging (67 %) |
| Measured 1-thread clock | 4.28 GHz | **4.83 GHz** |
| Files | `*_powersave.json`, `frequency_probe_powersave.log`, `environment.txt` | `cpu_sweep_*.json`, `frequency_probe.log`, `environment_bestperf.txt` |

Docker Desktop's containers were stopped before every measurement run
(`docker compose -f deploy/docker-compose.yml down`). Interpretation is in
`docs/notes/M2.4.md`. Defaults in force unless a command says otherwise:
`--warmup 5 --warmup-ms 500 --spin-ms 500 --cold clflush --trial-ms 50`.

| file | what it is / command (from the repo root) |
|---|---|
| `environment_bestperf.txt`, `environment.txt` | power state, load average, container state at the start of each run |
| `cpu_sweep_unpinned.json` | `bench run -w cpu_int,cpu_fp,cpu_hash -t 1,2,4,8,11,16,22 -n 30 --trial-ms 50` |
| `cpu_sweep_pinned.json` | same, plus `--pin` |
| `cpu_sweep_*_powersave.json` | the same two commands under Best power efficiency |
| `scaling_table.md` | the tables below, from `make_table.py` |
| `power_mode_ab.md` | the power-mode comparison, from `compare_power.py` |
| `disasm_kernels.log` | hot-loop disassembly with exact expected instruction counts |
| `trial_length_scaling.log` | the M1.2 iteration-scaling check on all three kernels |
| `frequency_probe.log`, `freq_probe.cpp` | effective core frequency, measured |
| `hash_fold.log` | the A/B study behind `cpu_hash`'s accumulator fold |
| `ctest_presets.log` | 97/97 tests in release, debug, ci, asan and tsan |
| `cli_and_schema.log` | `bench list`, `bench validate`, `check-jsonschema` |

`../cpu_2026-09-20.json` is a copy of the Best performance `cpu_sweep_unpinned.json` under
the name `PLAN.md` asks for (`docs/results/cpu_<date>.json`).

## Why the frequency here is measured, not read

Every `ops/cycle` figure needs a clock, and nothing inside WSL2 reports a usable one:
`/proc/cpuinfo` gives `cpu MHz : 3071.998` — the same value on all 22 vCPUs, which is
impossible on a hybrid part under turbo; `/sys/devices/system/cpu/cpu0/cpufreq` does not
exist; Windows reports only the 2.3 GHz nominal.

So `freq_probe.cpp` measures it: time a chain of instructions that must execute one after
another, where the latency in cycles is known. Two instructions with *different* known
latencies are used, because one chain cannot tell you whether you measured the clock or
measured the CPU optimising your chain away.

| chain | Best power efficiency | Best performance |
|---|---:|---:|
| `add reg,reg` (latency 1) | 4.363 GHz | 4.724 GHz |
| `imul reg,reg` (latency 3) | 4.200 GHz | 4.932 GHz |
| agreement | 3.9 % | 4.2 % |
| **used for ops/cycle** | **4.28 GHz** | **4.83 GHz** |
| `add $1,reg` | 25.6 G/s — *not a clock* | 27.1 G/s — *not a clock* |

The last row is the trap. The first version of this probe used `add $1, %rax` and reported
25 GHz. Recent Intel cores collapse chains of add-with-immediate to the same register in
the renamer, so that "dependency chain" was not one. Any single-chain probe would have
published 25 GHz without noticing.

## Target #1: aggregate `cpu_*_ops` ≥ 10M ops/s at the best thread count

Best performance, unpinned. PASS for all three, by two to three orders of magnitude.

| metric | best threads | aggregate ops/s | ≥ 1e7? | ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 16 | 4.011e+10 | **PASS** | 4.647e+09 | 0.96 |
| `cpu_fp_ops` | 22 | 1.000e+11 | **PASS** | 9.070e+09 | 1.88 |
| `cpu_hash_ops` | 16 | 1.244e+09 | **PASS** | 1.536e+08 | 0.032 |

The last column is the one worth reading, because all three values were predicted from the
disassembly before they were measured — and the re-run at a higher clock is what confirms
the prediction:

| kernel | predicted from the hot loop | predicted | at 4.28 GHz | at 4.83 GHz |
|---|---|---:|---:|---:|
| `cpu_int` | 8 `imul`/iteration ÷ one integer-multiply port | 1.00 | 0.90 (−10 %) | **0.96 (−4 %)** |
| `cpu_fp` | 8 lanes ÷ 4-cycle FMA latency | 2.00 | 1.85 (−8 %) | **1.88 (−6 %)** |
| `cpu_hash` | 31 `imul`/block on one multiply port | ≤ 0.032 | 0.030 | **0.032** |

Under Best power efficiency all three landed 7–10 % below prediction, consistently in the
same direction. The stated hypothesis was that the sustained clock during a 50 ms trial
sits below the peak the probe catches. Raising the power mode raised the sustained clock
and the gap closed to 4–6 %, with `cpu_hash` landing exactly on its port limit. That is the
hypothesis confirmed by an independent change, not by re-reading the same data.

## Power mode A/B

Same binary, same commands, ten minutes apart, only the Windows power mode changed.

| metric | best threads | Best power efficiency | Best performance | gain |
|---|---:|---:|---:|---:|
| `cpu_int_ops` | 16 | 3.711e+10 | 4.011e+10 | **+8.1 %** |
| `cpu_fp_ops` | 22 | 8.587e+10 | 1.000e+11 | **+16.5 %** |
| `cpu_hash_ops` | 16 | 1.083e+09 | 1.244e+09 | **+14.9 %** |

`PLAN.md` warns that a laptop on the wrong power plan "can swing 20 %". Measured here:
8–17 % on throughput and +13 % on the measured clock. Full per-thread-count table in
`power_mode_ab.md`.

The effect on **variance** is larger than the effect on throughput, and matters more for
M2.5:

| metric | 1-thread CoV, efficiency | 1-thread CoV, performance |
|---|---:|---:|
| `cpu_int_ops` | 15.0 % | **3.2 %** |
| `cpu_fp_ops` | 8.6 % | **3.9 %** |
| `cpu_hash_ops` | 12.3 % | **2.1 %** |

A power-managed laptop does not just run slower, it runs *unevenly* — the governor keeps
changing its mind mid-run. Roughly four fifths of the single-thread variance in the first
run was the power mode.

## Scaling across thread counts (Best performance)

Median of 30 trials. "Scaling eff." is `ops at N threads / (N × ops at 1 thread)`.

### unpinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV |
|---|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 4.647e+09 | 4.647e+09 | 100% | 3.2% |
| `cpu_int_ops` | 2 | 8.795e+09 | 4.397e+09 | 95% | 8.6% |
| `cpu_int_ops` | 4 | 1.658e+10 | 4.146e+09 | 89% | 8.9% |
| `cpu_int_ops` | 8 | 2.866e+10 | 3.583e+09 | 77% | 6.6% |
| `cpu_int_ops` | 11 | 3.245e+10 | 2.950e+09 | 63% | 5.1% |
| `cpu_int_ops` | 16 | **4.011e+10** | 2.507e+09 | 54% | 3.4% |
| `cpu_int_ops` | 22 | 3.937e+10 | 1.790e+09 | 39% | 8.4% |
| `cpu_fp_ops` | 1 | 9.070e+09 | 9.070e+09 | 100% | 3.9% |
| `cpu_fp_ops` | 2 | 1.733e+10 | 8.665e+09 | 96% | 3.0% |
| `cpu_fp_ops` | 4 | 3.369e+10 | 8.423e+09 | 93% | 2.7% |
| `cpu_fp_ops` | 8 | 6.126e+10 | 7.658e+09 | 84% | 1.4% |
| `cpu_fp_ops` | 11 | 7.580e+10 | 6.891e+09 | 76% | 1.7% |
| `cpu_fp_ops` | 16 | 9.843e+10 | 6.152e+09 | 68% | 2.5% |
| `cpu_fp_ops` | 22 | **1.000e+11** | 4.548e+09 | 50% | 9.5% |
| `cpu_hash_ops` | 1 | 1.536e+08 | 1.536e+08 | 100% | 2.1% |
| `cpu_hash_ops` | 2 | 2.946e+08 | 1.473e+08 | 96% | 5.5% |
| `cpu_hash_ops` | 4 | 5.192e+08 | 1.298e+08 | 84% | 8.5% |
| `cpu_hash_ops` | 8 | 9.140e+08 | 1.143e+08 | 74% | 7.7% |
| `cpu_hash_ops` | 11 | 1.036e+09 | 9.419e+07 | 61% | 4.6% |
| `cpu_hash_ops` | 16 | **1.244e+09** | 7.776e+07 | 51% | 3.4% |
| `cpu_hash_ops` | 22 | 1.235e+09 | 5.612e+07 | 37% | 7.5% |

### pinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV |
|---|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 4.593e+09 | 4.593e+09 | 100% | 4.7% |
| `cpu_int_ops` | 2 | 8.938e+09 | 4.469e+09 | 97% | 15.6% |
| `cpu_int_ops` | 4 | 1.672e+10 | 4.180e+09 | 91% | 8.0% |
| `cpu_int_ops` | 8 | 2.729e+10 | 3.411e+09 | 74% | 7.1% |
| `cpu_int_ops` | 11 | 3.210e+10 | 2.918e+09 | 64% | 5.5% |
| `cpu_int_ops` | 16 | 4.029e+10 | 2.518e+09 | 55% | 5.0% |
| `cpu_int_ops` | 22 | **4.081e+10** | 1.855e+09 | 40% | 9.0% |
| `cpu_fp_ops` | 1 | 9.461e+09 | 9.461e+09 | 100% | 3.0% |
| `cpu_fp_ops` | 2 | 1.808e+10 | 9.040e+09 | 96% | 3.4% |
| `cpu_fp_ops` | 4 | 3.389e+10 | 8.473e+09 | 90% | 3.0% |
| `cpu_fp_ops` | 8 | 6.082e+10 | 7.603e+09 | 80% | 2.8% |
| `cpu_fp_ops` | 11 | 7.547e+10 | 6.861e+09 | 73% | 1.4% |
| `cpu_fp_ops` | 16 | 9.733e+10 | 6.083e+09 | 64% | 1.2% |
| `cpu_fp_ops` | 22 | **1.044e+11** | 4.746e+09 | 50% | 7.0% |
| `cpu_hash_ops` | 1 | 1.463e+08 | 1.463e+08 | 100% | 2.9% |
| `cpu_hash_ops` | 2 | 2.895e+08 | 1.447e+08 | 99% | 3.7% |
| `cpu_hash_ops` | 4 | 5.061e+08 | 1.265e+08 | 87% | 9.0% |
| `cpu_hash_ops` | 8 | 9.438e+08 | 1.180e+08 | 81% | 4.5% |
| `cpu_hash_ops` | 11 | 1.072e+09 | 9.743e+07 | 67% | 5.6% |
| `cpu_hash_ops` | 16 | 1.250e+09 | 7.811e+07 | 53% | 3.7% |
| `cpu_hash_ops` | 22 | **1.321e+09** | 6.005e+07 | 41% | 5.8% |

Scaling is strongly sub-linear past ~4 threads and peaks at 16 of 22 threads for two of the
three metrics: 22 "CPUs" are 6 P-cores + 8 E-cores + 2 LP E-cores plus SMT, presented to
the guest as 11 identical cores × 2 threads. `PLAN.md` predicted flattening after ~6
threads; measured, it is closer to 11.

One artefact disappeared in the re-run. Under Best power efficiency, scaling efficiency at
2–4 threads read 104–109 %, which is impossible. It was the 1-thread baseline being
measured at a fluctuating clock. Under Best performance every efficiency is ≤ 100 %, which
is the expected shape and further evidence that the first run's baseline was the problem.

### Does pinning help? (correction)

The first run said pinning was 6–9 % *slower* and the earlier version of this note
generalised that. The re-run does not support the generalisation. Difference, pinned minus
unpinned, at the best thread count, in standard errors of the median:

| metric | Best power efficiency | Best performance |
|---|---|---|
| `cpu_int_ops` | −9.3 % (5.4 SE) | +0.4 % (0.3 SE) |
| `cpu_fp_ops` | −5.7 % (2.6 SE) | +4.4 % (1.6 SE) |
| `cpu_hash_ops` | +1.7 % (0.9 SE) | +0.5 % (0.4 SE) |

Under Best power efficiency pinning really did hurt `cpu_int` (5.4 SE is not noise). Under
Best performance no difference reaches 2 SE, so neither direction is supported. The
reading that fits both: when the package is power-limited, letting Linux move a thread off
a vCPU the host has throttled or descheduled is worth something; with power headroom there
is nothing to move away from. What stays true either way is the reason from `CLAUDE.md` —
guest-side pinning does not decide which *physical* core runs a thread — so pinning here
buys much less than it would on bare metal.

## Trial-length scaling (the M1.2 check, applied to all three)

From `trial_length_scaling.log` (taken under Best power efficiency; it is a ratio test, so
the power mode cancels). Double the trial length: the work must double, the rate must not
move.

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
noise, recorded rather than hidden; those runs were made under Best power efficiency, whose
measured 8–15 % single-thread CoV is a plausible contributor.
