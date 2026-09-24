# Computer Benchmarking Platform

A multithreaded C++20 benchmarking engine paired with a Go REST API for storing and
querying results at scale.

## What it does

- **Engine (C++20):** measures 12 CPU, memory, cache, and disk metrics (integer ops,
  floating point, hashing, memory bandwidth, memory latency, disk I/O). Recorded
  40.1 billion integer operations/sec across 16 threads on an Intel Core Ultra 9
  under WSL2.
- **API (Go + PostgreSQL + Redis):** ingests and serves benchmark results, sustaining
  1,000 requests/sec at 1.17ms p95 latency with zero errors during a 60-second k6
  load test, and serving 112K+ synthetic measurements.
- **Ingestion tuning:** batched `COPY` increased PostgreSQL ingestion throughput from
  4,437 to 26,611 rows/sec; streaming JSON ingestion cut retained decoding heap from
  11.5 MB to 2.3 MB.
- **Validation:** disk read throughput checked against `fio` within ±5% across tested
  configurations; 1,000-trial CPU variance studies used to identify thermal drift and
  host scheduling effects on repeatability.

## Structure

```
engine/     C++20 benchmarking engine (CMake, ctest)
api/        Go REST API, PostgreSQL migrations, k6 load-test scripts
deploy/     docker-compose for Postgres + Redis
schema/     JSON schema for benchmark results
docs/       per-milestone results and notes
```

## Running it

Requires Docker, Go, and a C++20 toolchain (CMake).

```bash
cp deploy/.env.example deploy/.env
make up            # start postgres + redis
make test          # run engine, api, and dashboard test suites
make bench         # run the engine and post results to the local API
```

See `make help` for the full list of commands, and `deploy/.env.loadtest.example`
for the load-test Postgres profile used to produce the throughput numbers above.

## Notes

- Default local credentials (`bench` / `bench`) are dev-only placeholders defined in
  `deploy/.env.example`, not production secrets.
- Per-milestone benchmark results and methodology notes live under `docs/results/`.
