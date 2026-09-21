# M1.3 — API: ingest and list

Raw output of the M1.3 verification, 2026-09-21. Reasoning: `docs/notes/M1.3.md`.

Machine: Core Ultra 9 185H under WSL2, plugged in, Windows power mode "Best performance"
(the same state as M2.4 onward). Postgres 16-alpine and Redis 7-alpine from
`deploy/docker-compose.yml`; the API run natively in WSL with `go run ./cmd/api`.

| file | what it is |
|---|---|
| `tests.log` | `go test ./...`, `go test -tags integration ./...`, `go vet`, and the per-test list |
| `ingest.log` | the end-to-end ingest check: POST, duplicate POST, row counts, list, health |
| `validation_bench.log` | strict decode + Go validator vs the normative JSON Schema, 50K results |
| `engine_run.json` | the real engine output that was posted (10 trials of `cpu_int`, 1 thread) |

## Commands

```bash
docker compose -f deploy/docker-compose.yml up -d --wait
cd api && go run ./cmd/migrate up
go run ./cmd/api &                     # :8080
cd ../engine && ./build/release/bench run --workload cpu_int --threads 1 --trials 10 --out run.json
curl -sS -X POST localhost:8080/v1/runs -H 'Content-Type: application/json' --data-binary @engine/run.json
```

## Results

| check | result |
|---|---|
| `go test ./...` | 15 tests, all pass |
| `go test -tags integration ./...` | 5 store tests against compose Postgres, all pass |
| first POST of a 10-result run | `{"inserted":10,"duplicate":false}` |
| second POST, same `run_id` | `{"inserted":0,"duplicate":true}` |
| `select count(*) from measurements` | 10 |
| `machines` after ingest | 1 row, id `7fea9eca…`, 11 physical cores, 24576 KiB L3 |
| unknown `?metric=` | HTTP 400, `{"error":{"code":"invalid_query",…}}` |
| strict decode + Go validator, 50K results | 98.3 ms, 91.8 MB, 120,037 allocs |
| JSON Schema at request time, 50K results | 1050.3 ms, 901.6 MB, 18,150,422 allocs |
