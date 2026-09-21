#!/usr/bin/env python3
"""Build the M2.4 scaling table from a run envelope. Numbers come from the file, never
from anywhere else; this script is committed so the table can be regenerated."""
import json, sys

FREQ_GHZ = 4.83  # measured, 1 thread, Best performance, docs/results/m2.4/frequency_probe.log

def load(path):
    d = json.load(open(path))
    by = {}
    for s in d["summary"]:
        by.setdefault(s["metric"], {})[s["thread_count"]] = s
    return d, by

def table(path, title):
    d, by = load(path)
    print(f"### {title}")
    print()
    print("| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV | ops/cycle/thread |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for metric, rows in by.items():
        one = rows[1]["median"]
        for n in sorted(rows):
            s = rows[n]
            per = s["median"] / n
            eff = s["median"] / (n * one)
            opc = per / (FREQ_GHZ * 1e9)
            print(f"| `{metric}` | {n} | {s['median']:.3e} | {per:.3e} | "
                  f"{eff*100:.0f}% | {s['cov']*100:.1f}% | {opc:.2f} |")
    print()

def best(path, title):
    d, by = load(path)
    print(f"### {title} — best thread count per metric (Target #1)")
    print()
    print("| metric | best threads | aggregate ops/s | >= 1e7? | per-thread ops/s at 1 thread | ops/cycle at 1 thread |")
    print("|---|---:|---:|:--:|---:|---:|")
    for metric, rows in by.items():
        bn = max(rows, key=lambda n: rows[n]["median"])
        b = rows[bn]["median"]
        one = rows[1]["median"]
        print(f"| `{metric}` | {bn} | {b:.3e} | {'PASS' if b >= 1e7 else 'FAIL'} | "
              f"{one:.3e} | {one/(FREQ_GHZ*1e9):.2f} |")
    print()

if __name__ == "__main__":
    print(f"Frequency used for ops/cycle: {FREQ_GHZ} GHz (measured, 1 thread).")
    print()
    best(sys.argv[1], "unpinned")
    table(sys.argv[1], "unpinned")
    if len(sys.argv) > 2:
        best(sys.argv[2], "pinned")
        table(sys.argv[2], "pinned")
