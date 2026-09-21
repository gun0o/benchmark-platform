#!/usr/bin/env python3
"""Power-mode A/B for M2.4: same binary, same commands, two Windows power modes."""
import json, sys

def by_metric(path):
    d = json.load(open(path))
    out = {}
    for s in d["summary"]:
        out.setdefault(s["metric"], {})[s["thread_count"]] = s
    return out

eff  = by_metric(sys.argv[1])   # Best power efficiency
perf = by_metric(sys.argv[2])   # Best performance
print("| metric | threads | Best power efficiency | Best performance | gain |")
print("|---|---:|---:|---:|---:|")
for m in perf:
    for n in sorted(perf[m]):
        a, b = eff[m][n]["median"], perf[m][n]["median"]
        print(f"| `{m}` | {n} | {a:.3e} | {b:.3e} | {(b/a-1)*100:+.1f}% |")
print()
print("Best thread count only:")
print()
print("| metric | best threads (perf) | efficiency | performance | gain |")
print("|---|---:|---:|---:|---:|")
for m in perf:
    bn = max(perf[m], key=lambda n: perf[m][n]["median"])
    a, b = eff[m][bn]["median"], perf[m][bn]["median"]
    print(f"| `{m}` | {bn} | {a:.3e} | {b:.3e} | {(b/a-1)*100:+.1f}% |")
