#!/usr/bin/env python3
"""M3.1 analysis: turn the run files into the tables quoted in docs/notes/M3.1.md.

Nothing here recomputes a value; every number is read out of the `summary` array the
engine wrote, so this script cannot disagree with the JSON.
"""
import json, pathlib

def summaries(name):
    return json.loads(pathlib.Path(name).read_text())["summary"]

def by(name, metric):
    return {s["working_set_bytes"]: s for s in summaries(name) if s["metric"] == metric}

def hs(b):
    for u, d in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if b >= d and b % d == 0:
            return f"{b // d} {u}"
    return f"{b} B"

print("## Working-set sweep, 1 thread (GB/s, median of 10 x 50 ms)\n")
r, w, c = (by("sweep_ws_1t.json", m) for m in
           ("mem_read_bw", "mem_write_bw", "mem_copy_bw"))
print("| working set | read | write | copy | write/read |")
print("|---|---:|---:|---:|---:|")
for ws in sorted(r):
    print(f"| {hs(ws)} | {r[ws]['median']:.1f} | {w[ws]['median']:.1f} | "
          f"{c[ws]['median']:.1f} | {w[ws]['median']/r[ws]['median']:.2f} |")

print("\n## Thread sweep at 256 MiB/thread, mem_read_bw\n")
print("| threads | GB/s | GB/s per thread | vs 1 thread | CoV % | late % |")
print("|---:|---:|---:|---:|---:|---:|")
ss = sorted(summaries("sweep_threads_read.json"), key=lambda s: s["thread_count"])
one = ss[0]["median"]
for s in ss:
    t = s["thread_count"]
    print(f"| {t} | {s['median']:.1f} | {s['median']/t:.2f} | {s['median']/one:.2f}x | "
          f"{s['cov']*100:.1f} | {100*s['late_trials']/s['n']:.0f} |")

print("\n## DRAM regime (256 MiB/thread): plain vs non-temporal stores\n")
print("| threads | stores | read | write | copy | write/read |")
print("|---:|---|---:|---:|---:|---:|")
for t, tag in ((1, ""), (8, "")):
    for nt, label in ((False, "plain"), (True, "non-temporal")):
        f = f"dram_rwc_{t}t{'_nt' if nt else ''}.json"
        m = {s["metric"]: s["median"] for s in summaries(f)}
        print(f"| {t} | {label} | {m['mem_read_bw']:.1f} | {m['mem_write_bw']:.1f} | "
              f"{m['mem_copy_bw']:.1f} | {m['mem_write_bw']/m['mem_read_bw']:.2f} |")

print("\n## Does glibc memcpy already use non-temporal stores?\n")
print("| working set | default threshold (24 MiB) | threshold forced to 1 GiB | change |")
print("|---|---:|---:|---:|")
d = by("nt_threshold_default.json", "mem_copy_bw")
f = by("nt_threshold_forced.json", "mem_copy_bw")
for ws in sorted(d):
    print(f"| {hs(ws)} | {d[ws]['median']:.2f} | {f[ws]['median']:.2f} | "
          f"{100*(f[ws]['median']/d[ws]['median']-1):+.1f} % |")
