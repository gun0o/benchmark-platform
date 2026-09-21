# M5.4 — k6 load test

Raw output of the M5.4 load tests, 2026-09-21, against the 112,513 rows M5.2 seeded.
Reasoning: `docs/notes/M5.4.md`.

Machine: Core Ultra 9 185H (22 vCPUs) under WSL2, plugged in, Windows power mode "Best
performance" — the same state as every other measurement in this repository. Postgres 16
and Redis 7 in compose, `synchronous_commit=on` (the default profile, **not**
`deploy/.env.loadtest.example`).

| file | what it is |
|---|---|
| `../k6_2026-09-21.json` | the headline run's `--summary-export`, written by `make load` |
| `../k6_2026-09-21.readyz.json` | the `/readyz` snapshot `make load` takes alongside it |
| `../k6_mixed_2026-09-21.json` | reads and ingest concurrently |
| `k6_compose_network.json` | variant (b): API, Postgres, Redis and k6 all on the compose network |
| `k6_no_redis.json` | the same load with the cache turned off |
| `k6_rate_*.json` | 2000 / 3000 / 5000 / 8000 / 12000 req/s |
| `k6_pool_*_rate6000.json` | `PG_MAX_CONNS` 10 / 20 / 40 / 80, cache off, 6000 req/s |
| `k6_after_mixed_412k_rows.json` | the read profile against the 412,513-row table the mixed run left |
| `summary.txt` | the table below, generated from those files by `api/loadtest/summarize.py` |
| `cpu.log` | CPU of api / k6 / postgres / redis during the 1000/s and 12000/s runs |

## Commands

```bash
make load                        # headline: writes docs/results/k6_<date>.json
make load-mixed                  # reads + ingest
cd api && RATE=5000 DURATION=30s PRE_VUS=400 k6 run loadtest/read_heavy.js
python3 api/loadtest/summarize.py
```

## Target #4

| requirement | measured |
|---|---|
| p(95) < 50 ms | **1.17 ms** |
| http_reqs ≥ 60,000 in 60 s | **60,001** in the measured window |
| dropped_iterations = 0 | **0** |
| error rate < 0.1 % | **0.00 %** |

Both variants of the run matrix pass: (a) API native in WSL with Postgres and Redis in
compose, k6 native; (b) API, Postgres, Redis and k6 all on the compose network.

## Every run

```
k6 --summary-export files, M5.4. 'measured' is the threshold window only
(the 10 s warmup scenario is excluded); 'all reqs' includes it.

| run                                                | measured | all reqs |  p50 ms |  p95 ms |   max ms | dropped | errors | cache hit |
|----------------------------------------------------|----------|----------|---------|---------|----------|---------|--------|-----------|
| (a) headline: 1000/s, 60 s, API native             |    60001 |    62003 |    0.38 |    1.17 |    54.22 |       0 |  0.00% |     95.9% |
| (b) all on the compose network, k6 in a container  |    60002 |    62004 |    0.35 |    1.12 |    21.16 |       0 |  0.00% |     95.0% |
| cache off (REDIS_URL=), 1000/s, 30 s               |    30001 |    32003 |    0.81 |    3.46 |    76.22 |       0 |  0.00% |      0.0% |
| mixed: 1000/s reads + 50/s ingest, 60 s            |    60001 |    65004 |    0.43 |    2.17 |   161.24 |       0 |  0.00% |     80.8% |
| after the mixed run, 412,513 rows, 1000/s          |    30000 |    32002 |    0.38 |    1.30 |   178.04 |       0 |  0.00% |     94.3% |
| 2000/s, preAllocatedVUs 100                        |    59954 |    63956 |    0.38 |    1.10 |    80.11 |      47 |  0.00% |     96.3% |
| 2000/s, preAllocatedVUs 400                        |    60001 |    64003 |    0.38 |    1.13 |    37.16 |       0 |  0.00% |     96.8% |
| 3000/s                                             |    89975 |    95977 |    0.36 |    1.09 |    53.21 |      26 |  0.00% |     98.2% |
| 5000/s                                             |   150001 |   160002 |    0.40 |    1.15 |    67.16 |       0 |  0.00% |     99.5% |
| 8000/s                                             |   239582 |   255585 |    0.51 |    1.49 |    95.48 |     419 |  0.00% |     99.2% |
| 12000/s                                            |   359401 |   383402 |    0.65 |    2.36 |   102.20 |     602 |  0.00% |     99.4% |
| pool 10, cache off, 6000/s                         |   119816 |   131817 |    3.97 |   41.49 |   229.91 |     186 |  0.00% |      0.0% |
| pool 20, cache off, 6000/s                         |   119960 |   131962 |    1.53 |   10.49 |   138.09 |      42 |  0.00% |      0.0% |
| pool 40, cache off, 6000/s                         |   119677 |   131679 |    1.48 |    9.17 |   190.51 |     324 |  0.00% |      0.0% |
| pool 80, cache off, 6000/s                         |   119314 |   131316 |    1.45 |    8.34 |   347.11 |     686 |  0.00% |      0.0% |
```

The request mix is 60 % `/v1/aggregates`, 20 % `/v1/compare`, 15 % `/v1/measurements`
(paged), 5 % `/v1/trials?limit=1000`, drawn from machine/metric/working-set combinations
fetched from the API in `setup()`.
