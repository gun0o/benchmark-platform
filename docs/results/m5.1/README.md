# M5.1 — Ingest and query hardening

Raw output of the M5.1 verification, 2026-09-21. Reasoning: `docs/notes/M5.1.md`.

Same machine and stack as M1.3: Core Ultra 9 185H under WSL2, Postgres 16-alpine in
compose (now with `shared_buffers=512MB`, `work_mem=32MB`, `synchronous_commit=on`), API
native in WSL.

| file | what it is |
|---|---|
| `ingest_50k.log` | a 50,000-result envelope over HTTP: time, repeat, 413, `synchronous_commit=off`, and the same rows written server-side |
| `ingest_paths.log` | row-by-row INSERT vs `CopyFrom` vs temp table + `ON CONFLICT`, with two diagnostics |
| `decode_bench.log` | whole-document vs streaming decode: time, allocation, retained heap |
| `explain.log` | `EXPLAIN (ANALYZE, BUFFERS)` for the list query, the keyset cursor, and `OFFSET` |
| `endpoints.log` | `/v1/machines`, `/v1/runs`, request ids, gzip, and the request log |
| `tests.log` | `go test ./...` and `go test -tags integration ./...` |

## Results

| check | result |
|---|---|
| 50,000 results (17.0 MB) POSTed over HTTP | **2.05 s** — target was < 2 s |
| the same envelope, in-process (12.0 MB fixture) | 1.64 s, 30,473 rows/s |
| the same 50,000 rows written inside Postgres, no client | 1.43 s |
| the same POST under `synchronous_commit=off` | 2.08 s (no change) |
| repeat POST of the whole run | 0.91 s, `inserted: 0, skipped: 50000` |
| 50,001 results | HTTP 413 `too_many_results` |
| row-by-row INSERT | 4,514 rows/s (11.1 s extrapolated to 50K) |
| `CopyFrom` in 10K batches | 27,711 rows/s |
| temp table + `ON CONFLICT DO NOTHING` | 19,458 rows/s |
| `CopyFrom` with the three indexes dropped | 33,307 rows/s |
| whole-document decode | 96.9 ms, 91.8 MB allocated, 11.5 MB retained |
| streaming decode into 10K batches | 96.3 ms, 21.1 MB allocated, **2.3 MB retained** |
| gzip on a 1000-row measurement page | 428,545 → 30,035 bytes (14.3×) |
| list query plan | Index Scan (primary key) — see the note in `explain.log`; re-checked in `m5.2/` |
| `OFFSET 40000` vs keyset cursor | 26.98 ms / 8388 buffers vs 0.16 ms / 26 buffers |
