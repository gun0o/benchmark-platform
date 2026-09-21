#!/usr/bin/env python3
"""Aggregate the alternating knob A/B (docs/results/m2.5/knobs/) into one table per knob.

Each knob setting was run 3 times, alternating with the other settings, so that the
machine's drift lands on every arm equally. The number reported per (setting, metric) is
the MEDIAN of the three repetitions' CoV, with the full spread shown, because a single
CoV estimate from 100 trials is itself noisy (its standard error is about CoV/sqrt(2n),
roughly 0.25 percentage points at CoV = 5%, and the repetitions disagree by more than
that - which is the point of repeating them).
"""
import json, statistics, sys
from pathlib import Path

D = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/results/m2.5/knobs")
METRICS = ["cpu_int_ops", "cpu_fp_ops", "cpu_hash_ops"]


def covs(pattern):
    """{metric: [cov per repetition]}"""
    out = {m: [] for m in METRICS}
    for f in sorted(D.glob(pattern)):
        for s in json.load(open(f))["summary"]:
            if s["metric"] in out:
                out[s["metric"]].append(s["cov"] * 100)
    return out


def emit(title, settings, note=""):
    print(f"\n### {title}\n")
    if note:
        print(note + "\n")
    print("| setting | " + " | ".join(f"`{m}`" for m in METRICS) + " |")
    print("|---|" + "---:|" * len(METRICS))
    data = {}
    for label, pattern in settings:
        c = covs(pattern)
        data[label] = c
        cells = []
        for m in METRICS:
            v = c[m]
            cells.append(f"{statistics.median(v):.2f}% ({min(v):.1f}-{max(v):.1f})" if v else "-")
        print(f"| {label} | " + " | ".join(cells) + " |")
    return data


if __name__ == "__main__":
    print(f"reps per setting: {len(list(D.glob('pin_on_*.json')))}"
          "  |  each cell: median CoV over repetitions (min-max)")
    emit("`--pin` on vs off", [("pinned", "pin_on_*.json"), ("unpinned", "pin_off_*.json")],
         "1 thread. Pinning a single worker sends it to vCPU 0, which also carries the "
         "guest's interrupt work.")
    emit("warmup on vs off",
         [("--warmup 20 --warmup-ms 500", "warmup_on_*.json"),
          ("--warmup 0 --warmup-ms 0 --spin-ms 0", "warmup_off_*.json")])
    emit("trial length",
         [("--trial-ms 10", "trialms_10_*.json"), ("--trial-ms 50", "trialms_50_*.json"),
          ("--trial-ms 200", "trialms_200_*.json")],
         "A longer trial averages more jitter inside each measurement, so CoV should fall. "
         "It also takes proportionally longer in wall time, so it collects more thermal "
         "drift - the two effects pull in opposite directions.")
