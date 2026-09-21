# M2.5 variance study — raw runs (2026-09-21)

Machine: Intel Core Ultra 9 185H, WSL2 (22 Hyper-V vCPUs), Ubuntu 22.04.5, g++ 13.4.0,
release preset (`-O3 -march=native -fno-omit-frame-pointer`).

**Power state is recorded before and after every single run** in `run_log.txt` and
`knob_ab_log.txt`, by `powerstate.sh`. All runs in this directory were taken **plugged in**,
Windows plan **Balanced**, power mode **Best performance** ("Max Performance Overlay"),
with Docker Desktop's containers stopped. M2.4 measured that the power mode alone changes
single-thread CoV by a factor of about four, so a variance study that did not record it
would not mean anything.

The conclusions are in `docs/notes/M2.5.md`; the headline table is
`docs/results/variance_2026-09-21.md`.

## Files

| file | what it is |
|---|---|
| `run_log.txt` | every command in pass 1, with power state and load average before and after |
| `knob_ab_log.txt` | the same for the second (alternating) knob pass |
| `powerstate.sh` | prints power line / battery / Windows power mode in one line |
| `run_study.sh` | pass 1: the Target #2 runs, the first-pass knob runs, the interleave comparison |
| `knob_ab.sh` | pass 2: the same knobs, alternated and repeated 3× |
| `analyze.py` | per-configuration statistics and noise diagnostics |
| `knob_table.py` | aggregates `knobs/` into one table per knob |
| `canary_demo.sh`, `canary_demo.log` | the clock-canary rule swept across load levels |
| `target2_cpu_*.json` | Target #2: 1000 trials, `--trial-ms 50 --warmup 20 --pin --cold clflush` |
| `knob_*.json` | pass-1 knob runs (200 trials each) |
| `interleave_o*.json` | `--interleave` on vs off, same configurations, 300 trials |
| `knobs/*.json` | pass-2 knob runs, 3 repetitions per setting, alternated |
| `canary_load_*.json` | the canary load sweep (0 / 1x / 3x / 10x oversubscribed) |
| `canary_sensitivity.log`, `canary_sens.cpp` | how wide a window the canary needs, and why mean/min is the signal |
| `analysis.txt`, `knob_table.txt` | generated output of the two analysis scripts |
| `schema_validation.log` | `bench validate` + `check-jsonschema` over all 38 run files |
| `report_target2.txt` | `bench report --wide` output for the Target #2 runs |

## Reading the diagnostic columns

`analyze.py` prints more than CoV, because "the CoV is 5%" does not say *why*. None of
these columns change a number; they describe the shape of the distribution the raw CoV
was computed over.

- **CoV** — sample stddev ÷ mean over the raw timed trials. This is Target #2. No
  trimming, ever.
- **MAD/med** — median absolute deviation over the median. For clean Gaussian noise
  `stddev ≈ 1.48 × MAD`; when CoV is much larger than MAD/med, the spread is a tail
  rather than the width of the distribution (the M2.2 finding).
- **late** — percentage of trials whose worker start spread exceeded 1000 µs, i.e. one
  worker was late off the barrier. That is host preemption, not the workload. Counted
  and reported, never dropped.
- **att** — how many times the configuration had to be run before one was kept, under the
  clock-canary rule.
- **CoV detrended** — a *diagnostic only*: CoV of the residuals after removing the
  least-squares trend. It answers "how much of the spread is slow drift versus
  trial-to-trial jitter". It is never the reported CoV, because a machine that drifts was
  genuinely not running at one speed.
- **sd/robust** — sample stddev ÷ (1.4826 × MAD). ≈ 1 means Gaussian; > 1.5 means tails.
- **drift** — least-squares slope across the run, as a total percentage change. Thermal
  decline shows up here and nowhere else.
- **slow frac / gap** — 1-D 2-means split: what fraction of trials fall in the slower
  cluster, and how far apart the two cluster centres are in units of the sample stddev.
  **A single mode is not 0** — calibration on pure Gaussian noise (in `analyze.py`) gives
  50% of trials at a 1.5σ gap, so that is the null result. Evidence of a real second mode
  is a gap well above 1.5σ or a split far from 50/50.
- **spikes** — trials more than 3 robust sigmas *below* the median. Throughput outliers
  are low (something stole time). Counted, not removed.
