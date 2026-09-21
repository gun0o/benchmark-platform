#!/usr/bin/env python3
"""Build the M5.4 results table from k6's --summary-export files.

Every number in docs/notes/M5.4.md and docs/results/m5.4/README.md comes out of this,
so the table cannot drift from the runs that produced it.

    python3 api/loadtest/summarize.py docs/results/k6_*.json docs/results/m5.4/*.json
"""
import json
import sys

# label, file
ROWS = [
    ("(a) headline: 1000/s, 60 s, API native", "docs/results/k6_2026-09-21.json"),
    ("(b) all on the compose network, k6 in a container", "docs/results/m5.4/k6_compose_network.json"),
    ("cache off (REDIS_URL=), 1000/s, 30 s", "docs/results/m5.4/k6_no_redis.json"),
    ("mixed: 1000/s reads + 50/s ingest, 60 s", "docs/results/k6_mixed_2026-09-21.json"),
    ("after the mixed run, 412,513 rows, 1000/s", "docs/results/m5.4/k6_after_mixed_412k_rows.json"),
    ("2000/s, preAllocatedVUs 100", "docs/results/m5.4/k6_rate_2000.json"),
    ("2000/s, preAllocatedVUs 400", "docs/results/m5.4/k6_rate_2000_prevus400.json"),
    ("3000/s", "docs/results/m5.4/k6_rate_3000.json"),
    ("5000/s", "docs/results/m5.4/k6_rate_5000.json"),
    ("8000/s", "docs/results/m5.4/k6_rate_8000.json"),
    ("12000/s", "docs/results/m5.4/k6_rate_12000.json"),
    ("pool 10, cache off, 6000/s", "docs/results/m5.4/k6_pool_10_rate6000.json"),
    ("pool 20, cache off, 6000/s", "docs/results/m5.4/k6_pool_20_rate6000.json"),
    ("pool 40, cache off, 6000/s", "docs/results/m5.4/k6_pool_40_rate6000.json"),
    ("pool 80, cache off, 6000/s", "docs/results/m5.4/k6_pool_80_rate6000.json"),
]

HEADER = (f"| {'run':<50} | {'measured':>8} | {'all reqs':>8} | {'p50 ms':>7} | {'p95 ms':>7} | "
          f"{'max ms':>8} | {'dropped':>7} | {'errors':>6} | {'cache hit':>9} |")
RULE = "|" + "|".join("-" * w for w in (52, 10, 10, 9, 9, 10, 9, 8, 11)) + "|"


def row(label, path):
    with open(path) as f:
        m = json.load(f)["metrics"]
    dur = m.get("http_req_duration{scenario:measured}") or m["http_req_duration"]
    reqs = m["http_reqs"]["count"]
    dropped = m.get("dropped_iterations", {}).get("count", 0)
    failed = m.get("http_req_failed{scenario:measured}") or m.get("http_req_failed", {})
    errors = failed.get("value", failed.get("rate", 0)) * 100
    # Requests inside the measured window (the warmup scenario is excluded from the
    # thresholds, and from this count): http_req_failed counts every request it judged.
    measured = failed.get("passes", 0) + failed.get("fails", 0)
    hits = m.get("cache_hits", {}).get("count", 0)
    misses = m.get("cache_misses", {}).get("count", 0)
    hitrate = f"{100 * hits / (hits + misses):.1f}%" if hits + misses else "n/a"
    return (f"| {label:<50} | {measured:>8} | {reqs:>8} | {dur['med']:>7.2f} | "
            f"{dur['p(95)']:>7.2f} | {dur['max']:>8.2f} | {dropped:>7} | {errors:>5.2f}% | {hitrate:>9} |")


def main(argv):
    print("k6 --summary-export files, M5.4. 'measured' is the threshold window only")
    print("(the 10 s warmup scenario is excluded); 'all reqs' includes it.")
    print()
    print(HEADER)
    print(RULE)
    for label, path in ROWS:
        try:
            print(row(label, path))
        except FileNotFoundError:
            print(f"| {label:<50} | {'missing':>7} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
