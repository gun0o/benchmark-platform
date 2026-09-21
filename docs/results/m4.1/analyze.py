#!/usr/bin/env python3
"""M4.1 analysis: the tables quoted in docs/notes/M4.1.md.

The engine's figures are read out of the `summary` array it wrote; fio's are read out of
fio's own JSON. Nothing here recomputes either side.
"""
import json, pathlib, statistics

def eng(name):
    return json.loads(pathlib.Path(name).read_text())

def by_metric(name, metric):
    return {s["thread_count"]: s for s in eng(name)["summary"] if s["metric"] == metric}

def fio_job(name):
    d = json.loads(pathlib.Path(name).read_text())
    return d["jobs"][0]

print("## Verify 2a: sequential 1 MiB O_DIRECT reads\n")
print("| threads | bench MB/s | fio MB/s | difference |")
print("|---:|---:|---:|---:|")
sr = by_metric("seq_read.json", "disk_seq_read_bw")
for t in sorted(sr):
    f = fio_job(f"fio_seqread_t{t}.json")["read"]
    fio_mb = f["bw_bytes"] / 1e6
    b = sr[t]["median"]
    print(f"| {t} | {b:.0f} | {fio_mb:.0f} | {100*(b/fio_mb-1):+.1f} % |")

print("\n## Verify 2b: random 4 KiB O_DIRECT reads, queue depth = threads\n")
print("| threads (QD) | bench IOPS | fio IOPS | diff | bench p99 us | fio p99 us | diff |")
print("|---:|---:|---:|---:|---:|---:|---:|")
ri = by_metric("rand_read.json", "disk_rand_read_iops")
rp = by_metric("rand_read.json", "disk_rand_read_p99_us")
for t in sorted(ri):
    f = fio_job(f"fio_randread_t{t}.json")["read"]
    fio_iops = f["iops"]
    # fio's completion-latency percentiles are in nanoseconds, keyed by percentile string.
    pct = f["clat_ns"]["percentile"]
    fio_p99 = pct["99.000000"] / 1000.0
    b, p = ri[t]["median"], rp[t]["median"]
    print(f"| {t} | {b:.0f} | {fio_iops:.0f} | {100*(b/fio_iops-1):+.1f} % | "
          f"{p:.0f} | {fio_p99:.0f} | {100*(p/fio_p99-1):+.1f} % |")

print("\n## Write metrics (no fio comparison: PLAN.md asks for reads)\n")
print("| metric | threads | median | unit | CoV % |")
print("|---|---:|---:|---|---:|")
for f, m, u in (("seq_write.json", "disk_seq_write_bw", "MB/s"),
                ("rand_write.json", "disk_rand_write_iops", "IOPS")):
    for t, s in sorted(by_metric(f, m).items()):
        print(f"| `{m}` | {t} | {s['median']:.0f} | {u} | {s['cov']*100:.1f} |")

print("\n## Verify 3: the cold check\n")
a = [r["value"] for r in eng("cold_trial0.json")["results"]]
b = [r["value"] for r in eng("cold_warmcache.json")["results"]]
print("| run | trial 0 | median of trials 1..9 | trial 0 / rest | min | max |")
print("|---|---:|---:|---:|---:|---:|")
for tag, v in (("no warmup, cold start", a), ("after the whole file was read into the page cache", b)):
    rest = statistics.median(v[1:])
    print(f"| {tag} | {v[0]:.0f} | {rest:.0f} | {v[0]/rest:.3f} | {min(v):.0f} | {max(v):.0f} |")
print(f"\nMedian over all ten trials: cold start {statistics.median(a):.0f} MB/s, "
      f"page cache deliberately filled {statistics.median(b):.0f} MB/s "
      f"({100*(statistics.median(b)/statistics.median(a)-1):+.1f} %).")

print("\n## Per-trial values, sequential read, no warmup (MB/s)\n")
print("```")
print("cold start : " + "  ".join(f"{x:.0f}" for x in a))
print("warm cache : " + "  ".join(f"{x:.0f}" for x in b))
print("```")

print("\n## Verify 2c: engine vs fio, three alternating repetitions\n")
print("A single A-then-B comparison confounds the tool with when it ran (M2.5's lesson), and\n"
      "the Windows host's cache over the VHDX is exactly the kind of thing that drifts.\n")
print("| measurement | rep | bench | fio | bench/fio |")
print("|---|---:|---:|---:|---:|")
rows = {}
for t in (1, 4):
    for rep in (1, 2, 3):
        b = eng(f"alt_bench_seq_t{t}_{rep}.json")["summary"][0]["median"]
        f = fio_job(f"alt_fio_seq_t{t}_{rep}.json")["read"]["bw_bytes"] / 1e6
        rows.setdefault(f"seq read MB/s, {t} thread(s)", []).append((rep, b, f))
for t in (1, 4, 16):
    for rep in (1, 2, 3):
        s = [x for x in eng(f"alt_bench_rand_t{t}_{rep}.json")["summary"]
             if x["metric"] == "disk_rand_read_iops"][0]["median"]
        f = fio_job(f"alt_fio_rand_t{t}_{rep}.json")["read"]["iops"]
        rows.setdefault(f"rand read IOPS, QD {t}", []).append((rep, s, f))
for name, vals in rows.items():
    for rep, b, f in vals:
        print(f"| {name} | {rep} | {b:.0f} | {f:.0f} | {b/f:.3f} |")
print()
print("| measurement | median bench/fio | range |")
print("|---|---:|---|")
for name, vals in rows.items():
    r = sorted(b / f for _, b, f in vals)
    print(f"| {name} | **{statistics.median(r):.3f}** | {r[0]:.3f}-{r[-1]:.3f} |")
