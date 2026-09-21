# M1.4 — Engine posts to API

Raw output of the M1.4 verification, 2026-09-21. Reasoning: `docs/notes/M1.4.md`.

| file | what it is |
|---|---|
| `post.log` | the live `--post` round trip, plus every failure mode (refused, https, 404, `--out` as well) |
| `tests.log` | `ctest --preset release` (178 tests) and the ten new HTTP-client tests |

## Commands

```bash
cd engine && cmake --build --preset release -j
./build/release/bench run --workload cpu_int --threads 1 --trials 10 --post http://localhost:8080
```

## Results

| check | result |
|---|---|
| `measurements` before / after | 10 → 20 |
| engine's own line | `bench: POST http://localhost:8080/v1/runs -> 200 (10 results, 9576 bytes)` |
| API reply, printed to stdout | `{"run_id":"31085d5d…","inserted":10,"duplicate":false}` |
| API log line | `"msg":"ingested run","inserted":10,"duplicate":false` |
| `--post` to a dead port | `connect: Connection refused`, exit 1, `--out` file still complete |
| `--post https://…` | refused by name, exit 1 |
| `--post` to a wrong path | `-> 404`, prints the server's body, exit 1 |
| `--post` + `--out` together | both happen |
| engine test suite | 178/178 pass, including 10 new HTTP tests |
