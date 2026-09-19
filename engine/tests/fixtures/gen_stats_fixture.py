#!/usr/bin/env python3
"""Generate tests/fixtures/stats.json: reference statistics computed with numpy.

The C++ stats library must reproduce these to a relative tolerance of 1e-9:
  mean, std (ddof=1, i.e. sample standard deviation), median, p5, p95 (numpy 'linear'
  percentile interpolation), min, max, cov (= std / mean), mad (median absolute deviation).
Deterministic: seed 20260918. Re-run this script only when adding cases.
"""
import json
import numpy as np

rng = np.random.default_rng(20260918)
cases = []

def add(name, a):
    a = np.asarray(a, dtype=np.float64)
    med = float(np.median(a))
    cases.append({
        "name": name,
        "values": [float(repr(float(x)) and x) for x in a],
        "n": int(a.size),
        "mean": float(np.mean(a)),
        "std": float(np.std(a, ddof=1)),
        "median": med,
        "p5": float(np.percentile(a, 5)),
        "p95": float(np.percentile(a, 95)),
        "min": float(np.min(a)),
        "max": float(np.max(a)),
        "cov": float(np.std(a, ddof=1) / np.mean(a)),
        "mad": float(np.median(np.abs(a - med))),
    })

add("two_values", [1.0, 3.0])
add("small_odd", rng.uniform(10, 20, 7))
add("small_even", rng.uniform(10, 20, 8))
add("uniform_100", rng.uniform(0.5, 1.5, 100))
add("lognormal_1000", rng.lognormal(mean=21.0, sigma=0.02, size=1000))       # ~1.3e9 ops/s with 2% noise
add("lognormal_1001", rng.lognormal(mean=21.0, sigma=0.02, size=1001))       # odd n: median is an element
add("large_offset_cancellation", 1e9 + rng.normal(0, 1, 500))               # naive sum-of-squares would lose precision
add("bimodal", np.concatenate([rng.normal(1.0, 0.01, 300), rng.normal(2.0, 0.01, 300)]))
add("heavy_tail", rng.pareto(3.0, 400) + 1.0)
add("sorted_ascending", np.arange(1.0, 51.0))

with open("stats.json", "w") as f:
    json.dump({"generator": "gen_stats_fixture.py", "numpy": np.__version__, "cases": cases}, f, indent=1)
print(f"wrote {len(cases)} cases")
