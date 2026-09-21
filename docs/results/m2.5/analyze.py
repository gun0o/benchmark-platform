#!/usr/bin/env python3
"""M2.5 variance analysis. Reads run envelopes and prints the tables that go into
docs/results/variance_<date>.md.

Nothing here trims, filters or re-weights a trial. Every statistic is over the raw timed
values exactly as the engine emitted them; the diagnostic columns describe the shape of
that distribution, they do not change it.
"""
import json, math, statistics, sys
from pathlib import Path

TARGET = 0.03  # Target #2: CoV <= 3%


def configs(path):
    """Yield (summary, [trial values in order]) per configuration in a run file."""
    d = json.load(open(path))
    for s in d["summary"]:
        vals = [r["value"] for r in d["results"]
                if r["metric"] == s["metric"] and r["thread_count"] == s["thread_count"]
                and r["working_set_bytes"] == s["working_set_bytes"]]
        yield s, vals


def robust_sigma(v):
    """MAD rescaled to a standard deviation. For clean Gaussian noise this equals the
    sample stddev; when it is much smaller, the spread is coming from a tail."""
    m = statistics.median(v)
    return 1.4826 * statistics.median([abs(x - m) for x in v])


def drift_pct(v):
    """Least-squares slope over the trial index, expressed as the total percentage change
    across the run. A steady thermal decline shows up here and nowhere else."""
    n = len(v)
    if n < 3:
        return 0.0
    xs = list(range(n))
    mx, my = (n - 1) / 2, statistics.fmean(v)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, v))
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0 or my == 0:
        return 0.0
    return (num / den) * (n - 1) / my * 100.0


# 1-D 2-means always splits a sample in two, including a sample with only one mode, so the
# split on its own proves nothing. Calibration on pure Gaussian noise (seed 7, n=1000, 20
# repetitions per CoV) gives the null result this is read against:
#
#     CoV  2% -> 50.3% of trials in the slow cluster, centres 1.57 sigma apart
#     CoV  5% -> 50.3%,                                        1.54 sigma
#     CoV 10% -> 50.5%,                                        1.48 sigma
#
# So one mode looks like "50% at 1.5 sigma". Evidence of a *second* mode is a gap well
# above 1.5 sigma, or a split far from 50/50 (a minority of trials sitting apart from the
# rest). Reporting the gap in sigma rather than in per cent is what makes that comparison
# possible at all.
GAUSSIAN_GAP_SIGMA = 1.53
GAUSSIAN_SLOW_FRAC = 50.3


def cov_detrended(v):
    """CoV of the residuals after removing the least-squares trend.

    This is a DIAGNOSTIC, not a result. Target #2 is judged on the raw CoV; this number
    exists only to answer "how much of the spread is the slow drift, and how much is
    trial-to-trial jitter?". Subtracting a trend is not outlier trimming - no trial is
    dropped and every trial still contributes - but it does describe a machine that was
    never actually running at a steady speed, so it must never be quoted as the CoV."""
    n = len(v)
    if n < 3:
        return 0.0
    xs = list(range(n))
    mx, my = (n - 1) / 2, statistics.fmean(v)
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0 or my == 0:
        return 0.0
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, v)) / den
    resid = [y - slope * (x - mx) for x, y in zip(xs, v)]
    return statistics.stdev(resid) / statistics.fmean(resid)


def bimodality(v):
    """One pass of 1-D 2-means. Returns (fraction in the slow cluster, gap between cluster
    centres as % of the fast centre). Read against the calibration above, never alone."""
    lo, hi = min(v), max(v)
    if hi == lo:
        return 0.0, 0.0
    c_lo, c_hi = lo, hi
    for _ in range(50):
        a = [x for x in v if abs(x - c_lo) <= abs(x - c_hi)]
        b = [x for x in v if abs(x - c_lo) > abs(x - c_hi)]
        if not a or not b:
            return 0.0, 0.0
        n_lo, n_hi = statistics.fmean(a), statistics.fmean(b)
        if n_lo == c_lo and n_hi == c_hi:
            break
        c_lo, c_hi = n_lo, n_hi
    slow = [x for x in v if abs(x - c_lo) <= abs(x - c_hi)]
    return len(slow) / len(v), (c_hi - c_lo) / c_hi * 100.0


def spikes(v):
    """Trials more than 3 robust sigmas below the median. Throughput outliers are low
    (something stole time); this counts them without removing them."""
    m, s = statistics.median(v), robust_sigma(v)
    return 0 if s == 0 else sum(1 for x in v if x < m - 3 * s)


def row(s, v):
    med = s["median"]
    rs = robust_sigma(v)
    frac, gap = bimodality(v)
    sd_pct = s["stddev"] / med * 100 if med else 0
    return {
        "gap_sigma": gap / sd_pct if sd_pct else 0,
        "metric": s["metric"], "threads": s["thread_count"], "n": s["n"],
        "median": med, "cov": s["cov"], "mad_med": s["mad"] / med * 100 if med else 0,
        "sd_over_robust": s["stddev"] / rs if rs else float("inf"),
        "late_pct": 100.0 * s.get("late_trials", 0) / s["n"] if s["n"] else 0,
        "attempts": s.get("canary_attempts", 1),
        "clk0": s.get("clock_call_ns_start", 0), "clk1": s.get("clock_call_ns_end", 0),
        "drift": drift_pct(v), "slow_frac": frac * 100, "mode_gap": gap, "spikes": spikes(v),
        "cov_detrended": cov_detrended(v),
    }


def table(rows, wide=False):
    hdr = f"| config | n | median | CoV | MAD/med | late | att |"
    sep = "|---|---:|---:|---:|---:|---:|---:|"
    if wide:
        hdr += " CoV detrended | sd/robust | drift | slow frac | gap | spikes |"
        sep += "---:|---:|---:|---:|---:|---:|"
    out = [hdr, sep]
    for r in rows:
        line = (f"| `{r['metric']}`@{r['threads']} | {r['n']} | {r['median']:.4g} | "
                f"{'**' if r['cov'] <= TARGET else ''}{r['cov']*100:.2f}%"
                f"{'**' if r['cov'] <= TARGET else ''} | {r['mad_med']:.2f}% | "
                f"{r['late_pct']:.1f}% | {r['attempts']} |")
        if wide:
            line += (f" {r['cov_detrended']*100:.2f}% | {r['sd_over_robust']:.2f} | {r['drift']:+.2f}% | "
                     f"{r['slow_frac']:.0f}% | {r['gap_sigma']:.2f}s | {r['spikes']} |")
        out.append(line)
    return "\n".join(out)


def collect(path):
    return [row(s, v) for s, v in configs(path)]


if __name__ == "__main__":
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/results/m2.5")
    for f in sorted(d.glob("*.json")):
        rows = collect(f)
        print(f"\n### {f.stem}\n")
        print(table(rows, wide=True))
