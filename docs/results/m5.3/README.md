# M5.3 — Aggregation, compare, Redis cache

Raw output of the M5.3 verification, 2026-09-21, over the 112,510 rows M5.2 seeded.
Reasoning: `docs/notes/M5.3.md`.

| file | what it is |
|---|---|
| `endpoints.log` | `/v1/aggregates`, `/v1/compare`, `/v1/trials`, downsampling, and the 400s |
| `cache.log` | hit/miss timings, what Redis holds, per-machine invalidation, Redis stopped |
| `tests.log` | `go test ./...`, `go test -tags integration ./...`, and the numpy cross-check |

## Results

| check | result |
|---|---|
| SQL aggregates vs numpy over the same rows | 25 groups across 4 metrics, **agree to better than 1e-6 relative** |
| cache hit p50 (aggregates, 6 groups) | **0.48 ms** (p95 0.66 ms) against a 6.68 ms miss — 13.8× |
| cache hit p50 (trials, 100 points) | **0.47 ms** against a 2.74 ms miss — 5.8× |
| keys after 5 distinct queries | 5 value keys + 2 per-machine sets, one key per distinct query |
| ingest for synthetic-02 | evicted synthetic-02's 2 keys **and** the compare key that depends on it; synthetic-01's keys still hit |
| `docker compose stop redis` | every endpoint still 200, `/readyz` 200 with `"redis":"degraded"` |
| latency with Redis down, **before** the circuit breaker | 3.4 s per request |
| latency with Redis down, after it | 0.81 s, 0.81 s, then **1.3 ms** |
| `/v1/trials` full vs `lttb:20` vs gzip | 5,468 → 1,203 → 1,695 bytes |
| aggregate without `machine_id` or `metric` | HTTP 400 `missing_filter` |
