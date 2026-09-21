#!/usr/bin/env python3
"""M3.2 analysis: the tables quoted in docs/notes/M3.2.md.

Nothing is recomputed from raw trial values except where explicitly said so: the per-run
figures are read out of the `summary` array the engine wrote.
"""
import json, pathlib, statistics

def summaries(name):
    return json.loads(pathlib.Path(name).read_text())["summary"]

def hs(b):
    for u, d in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if b >= d and b % d == 0:
            return f"{b // d} {u}"
    return f"{b} B"

# ---- Verify 1: the sweep, five independent passes ------------------------------------
reps = [ {s["working_set_bytes"]: s["median"] for s in summaries(f"sweep_rep{r}.json")}
         for r in range(1, 6) ]
sizes = sorted(reps[0])

print("## Working-set sweep, 1 thread, warm (`--cold none`), ns per dependent load\n")
print("Five independent passes, each 10 trials of 30 ms. `med` is the median of the five "
      "pass medians.\n")
print("| working set | rep 1 | rep 2 | rep 3 | rep 4 | rep 5 | med | min | max | max/min |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for ws in sizes:
    v = [r[ws] for r in reps]
    print(f"| {hs(ws)} | " + " | ".join(f"{x:.2f}" for x in v) +
          f" | **{statistics.median(v):.2f}** | {min(v):.2f} | {max(v):.2f} | "
          f"{max(v)/min(v):.1f}x |")

print("\n## Verify 3: huge pages on vs off\n")
print("Three alternating pairs, so a drift affects both sides equally. Median of the three.\n")
print("| working set | huge pages | 4 KiB pages | penalty |")
print("|---|---:|---:|---:|")
h = [ {s["working_set_bytes"]: s["median"] for s in summaries(f"huge_rep{r}.json")}
      for r in (1, 2, 3) ]
n = [ {s["working_set_bytes"]: s["median"] for s in summaries(f"nohuge_rep{r}.json")}
      for r in (1, 2, 3) ]
for ws in sorted(h[0]):
    hv = statistics.median(x[ws] for x in h)
    nv = statistics.median(x[ws] for x in n)
    print(f"| {hs(ws)} | {hv:.1f} | {nv:.1f} | {100*(nv/hv-1):+.0f} % |")

print("\n## Verify 2: 1 MiB, 200 trials of 50 ms, pinned\n")
print("| cold mode | median ns | mean ns | CoV % | MAD/median % | min | max |")
print("|---|---:|---:|---:|---:|---:|---:|")
for tag, f in (("`--cold clflush`", "cov_1m_cold.json"), ("`--cold none`", "cov_1m_warm.json")):
    s = summaries(f)[0]
    print(f"| {tag} | {s['median']:.2f} | {s['mean']:.2f} | {s['cov']*100:.2f} | "
          f"{100*s['mad']/s['median']:.2f} | {s['min']:.2f} | {s['max']:.2f} |")

print("\n## Cold vs warm across working sets (10 trials of 30 ms)\n")
print("| working set | `--cold none` | `--cold clflush` | ratio | params.cold |")
print("|---|---:|---:|---:|---|")
c = {s["working_set_bytes"]: s for s in summaries("coldmode_clflush.json")}
w = {s["working_set_bytes"]: s for s in summaries("coldmode_none.json")}
eff = {}
for r in json.loads(pathlib.Path("coldmode_clflush.json").read_text())["results"]:
    eff.setdefault(r["working_set_bytes"], r["params"]["cold"])
for ws in sorted(c):
    print(f"| {hs(ws)} | {w[ws]['median']:.2f} | {c[ws]['median']:.2f} | "
          f"{c[ws]['median']/w[ws]['median']:.2f}x | `{eff[ws]}` |")
