# benchmark-platform

A computer benchmarking platform: a C++20 engine measures CPU, memory, cache and disk
performance and emits JSON; a Go API persists and queries the measurements; a React
dashboard visualizes and compares hardware. Everything runs locally with docker-compose.

Portfolio project. Every performance claim in this repo is a **measured** number with a
recorded methodology, never an assumed one. See `PLAN.md` for the build order and
`docs/results/` (once it exists) for real numbers.

## Architecture

```
 ┌──────────────┐  JSON (file or HTTP POST)  ┌─────────────┐   SQL    ┌────────────┐
 │  engine/     │ ─────────────────────────▶ │  api/       │ ───────▶ │ PostgreSQL │
 │  C++20 CLI   │                            │  Go REST    │          └────────────┘
 │  (runs on    │                            │  :8080      │   cache  ┌────────────┐
 │   bare host) │                            │             │ ───────▶ │   Redis    │
 └──────────────┘                            └──────▲──────┘          └────────────┘
                                                    │ JSON over HTTP
                                             ┌──────┴──────┐
                                             │ dashboard/  │
                                             │ React + TS  │
                                             │ Recharts    │
                                             └─────────────┘
```

Data flow: `engine run` produces one **run** (a machine snapshot plus a flat list of
**results**, one per trial per metric per configuration). The API ingests runs in bulk,
stores them normalized (`machines`, `runs`, `measurements`), and serves raw and
aggregated (mean / median / p95 / CoV) views. The dashboard reads only the API.

The engine is a native binary. It runs on the host, not in a container, because Docker
adds scheduling and I/O virtualization noise. The API, Postgres, Redis and dashboard run
in docker-compose. WSL2 itself is a VM; see "Environment caveats".

## Folder layout

```
benchmark-platform/
├── CLAUDE.md                 # this file
├── PLAN.md                   # phased milestones
├── README.md                 # (later) user-facing overview + real results
├── Makefile                  # (later) root shortcuts: make up / test / bench / seed / load
├── schema/
│   └── benchmark-result.schema.json   # JSON Schema (draft 2020-12) for one result; source of truth
├── docs/
│   ├── methodology.md        # (later) how each metric is measured, verbatim from PLAN.md
│   └── results/              # (later) committed engine output from real runs (JSON + summary)
├── engine/                   # C++20, CMake
│   ├── CMakeLists.txt
│   ├── CMakePresets.json     # debug / release / asan / tsan presets
│   ├── include/bench/        # public headers (one per concern)
│   │   ├── timing.hpp        # steady_clock helpers, DoNotOptimize, ClobberMemory
│   │   ├── affinity.hpp      # pin_to_cpu, topology query
│   │   ├── cache.hpp         # flush_lines (clflushopt), evict_llc, huge-page alloc
│   │   ├── stats.hpp         # Welford, median, percentile, CoV
│   │   ├── workload.hpp      # Workload concept + registry
│   │   ├── runner.hpp        # trial loop, thread pool, barrier/latch sync
│   │   ├── sysinfo.hpp       # cpu model, cache sizes, memory, kernel
│   │   └── result.hpp        # Result struct <-> JSON
│   ├── src/
│   │   ├── main.cpp          # CLI (CLI11)
│   │   ├── runner.cpp
│   │   ├── stats.cpp
│   │   ├── sysinfo.cpp
│   │   ├── cache.cpp
│   │   ├── result.cpp
│   │   └── workloads/
│   │       ├── cpu_int.cpp   # cpu_int_ops
│   │       ├── cpu_fp.cpp    # cpu_fp_ops
│   │       ├── cpu_hash.cpp  # cpu_hash_ops
│   │       ├── mem_bw.cpp    # mem_read_bw, mem_write_bw, mem_copy_bw
│   │       ├── mem_latency.cpp  # mem_latency
│   │       └── disk_io.cpp   # disk_* (O_DIRECT)
│   └── tests/                # GoogleTest; one file per header
├── api/                      # Go 1.23+
│   ├── go.mod
│   ├── cmd/
│   │   ├── api/main.go       # HTTP server
│   │   ├── seed/main.go      # synthetic data generator (100K+ measurements)
│   │   └── migrate/main.go   # runs migrations (or use golang-migrate CLI)
│   ├── internal/
│   │   ├── http/             # handlers, routing (net/http ServeMux), middleware
│   │   ├── store/            # pgx repository, COPY ingest, queries
│   │   ├── cache/            # Redis cache-aside for aggregates
│   │   ├── model/            # Go structs mirroring schema/benchmark-result.schema.json
│   │   └── stats/            # server-side aggregation helpers (if not in SQL)
│   ├── migrations/           # NNNN_name.up.sql / .down.sql
│   ├── openapi.yaml          # API contract; dashboard types are generated from it
│   └── loadtest/             # k6 scripts + thresholds
├── dashboard/                # Vite + React 18 + TypeScript + Recharts + TanStack Query
│   ├── package.json
│   ├── src/
│   │   ├── api/              # generated types (openapi-typescript) + fetch client
│   │   ├── charts/           # one component per chart family
│   │   ├── pages/            # Overview, Metric detail, Compare, Trials
│   │   ├── lib/              # LTTB downsampling, formatting, units
│   │   └── main.tsx
│   └── e2e/                  # Playwright smoke tests
└── deploy/
    ├── docker-compose.yml    # postgres, redis, api, dashboard (nginx)
    ├── docker/               # Dockerfiles (api, dashboard, engine-builder)
    ├── aws/                  # Terraform: ECR, ECS Fargate, RDS, ElastiCache, ALB
    └── .env.example
```

## Build / test commands

Run from the repo root unless noted. Placeholders marked `(planned)` do not exist yet.

### Everything

```bash
docker compose -f deploy/docker-compose.yml up -d        # postgres + redis (+ api + dashboard once built)
docker compose -f deploy/docker-compose.yml down -v      # tear down incl. volumes
make up / make test / make bench / make seed / make load  # (planned) root Makefile
```

### engine/ (C++20, CMake ≥ 3.22, g++ 13)

```bash
cd engine
cmake --preset release                      # configure (Ninja if available, else Make)
cmake --build --preset release -j            # build -> build/release/bench
ctest --preset release --output-on-failure   # unit tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # sanitizer run
./build/release/bench list                   # list workloads and metrics
./build/release/bench sysinfo                # print machine block as JSON
./build/release/bench run --workload cpu_int --threads 1,2,4,8,16 --trials 30 --out run.json
./build/release/bench run --workload cpu_int --trials 12 --warmup 0 --spin-ms 0 --verbose   # see the warmup effect
./build/release/bench run --workload mem_latency --working-set 16K,64K,1M,8M,64M,512M --cold clflush
./build/release/bench run --all --trials 1000 --trial-ms 50 --pin --cold clflush --post http://localhost:8080
./build/release/bench validate run.json     # check output against schema/
```

Dependencies are fetched with CMake `FetchContent` (nlohmann/json, CLI11, GoogleTest).
No system packages beyond the toolchain.

### api/ (Go 1.23+)

Go is installed user-locally at `~/.local/opt/go` (no sudo on this machine); the root
`Makefile` prepends it to PATH, as does `~/.bashrc`.

```bash
cd api
go build ./...
go vet ./... && golangci-lint run           # lint
go test ./...                               # unit tests
go test -tags integration ./...             # needs postgres+redis from docker-compose
go run ./cmd/migrate up
go run ./cmd/api                            # :8080, reads DATABASE_URL / REDIS_URL
go run ./cmd/seed --machines 5 --trials 100 --seed 42   # writes 100K+ synthetic measurements
k6 run loadtest/read_heavy.js               # load test (needs seeded DB)
```

### dashboard/ (Node 20+, npm)

```bash
cd dashboard
npm ci
npm run dev                                 # Vite dev server :5173, proxies /v1 -> :8080
npm run build                               # production bundle -> dist/
npm run typecheck && npm run lint
npm run test                                # Vitest
npm run gen:api                             # regenerate src/api/types.ts from ../api/openapi.yaml
npm run e2e                                 # Playwright (needs api + seeded DB)
```

## Coding conventions

### General
- Small, reviewable commits on feature branches; PR into `main`. Conventional commit prefixes
  (`engine:`, `api:`, `dashboard:`, `deploy:`, `docs:`).
- Every milestone in `PLAN.md` ends with a verification step; do not mark it done until
  the verification has actually been run and its output recorded.
- Numbers in README/docs come from committed files under `docs/results/`, never typed by hand.
- Schema changes go through `schema/benchmark-result.schema.json` first, then the engine
  writer, then the Go model, then `openapi.yaml`, then generated TS types. Bump
  `schema_version` on any breaking change.

### C++ (engine)
- C++20, `-std=c++20`, no modules, no exceptions in hot paths (exceptions are fine for
  CLI/setup errors). Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
- Release flags: `-O3 -march=native -fno-omit-frame-pointer`. Never `-ffast-math`
  (it changes what an FP "operation" means). `-march=native` is recorded in the result's
  `machine.compiler_flags` field because it makes binaries machine-specific.
- Use modern features where they help, not for show: `std::jthread` + `std::stop_token`
  for workers, `std::barrier` for synchronized trial start, `std::latch` for completion,
  `std::span` for buffers, `std::format` for text output, concepts for the `Workload`
  interface, ranges for stats helpers, `std::bit_cast` for hash mixing.
- Timing uses `std::chrono::steady_clock` only. Never `system_clock` or `high_resolution_clock`.
- Per-thread mutable state lives in structs padded to 128 bytes (`alignas(128)`), not
  `std::hardware_destructive_interference_size` (GCC warns, and Intel's adjacent-line
  prefetcher makes 128 the safe value).
- Every benchmark loop passes its result through `DoNotOptimize()` (an `asm volatile` sink)
  and inputs derive from runtime values (CLI args / RNG seeded at runtime), so the compiler
  cannot constant-fold or dead-code-eliminate the work.
- Formatting: `clang-format` (LLVM base, 100 cols, 4-space indent). `snake_case` for
  functions/variables, `PascalCase` for types, `k`-prefixed constants (`kCacheLine`).
- Tests: GoogleTest, one test file per header. Stats functions are tested against
  fixtures computed with numpy (fixtures committed as JSON).

### Go (api)
- Standard layout: `cmd/` binaries, `internal/` packages. `gofmt`, `go vet`,
  `golangci-lint` clean.
- `net/http` with Go 1.22+ method-pattern routing; no framework. `log/slog` for logs.
- `pgx/v5` with a pool; bulk ingest uses `CopyFrom`. Hand-written SQL in `internal/store`,
  one function per query. Migrations are plain SQL files, applied by `golang-migrate`.
- Errors are wrapped with `%w` and context. Handlers return a JSON error envelope
  `{"error": {"code": "...", "message": "..."}}`.
- Redis is cache-aside for aggregate/compare queries with TTL, invalidated per-machine on
  ingest. The API must work with Redis down (log + fall through to Postgres).
- Table-driven tests. Integration tests behind `//go:build integration` and use the
  docker-compose Postgres/Redis.
- Expose `/healthz`, `/readyz`, `/metrics` (Prometheus), and `/debug/pprof` (non-prod).

### TypeScript (dashboard)
- `strict: true`, ESLint + Prettier, no `any`. API types are generated, never hand-written.
- Server state via TanStack Query; no global state library unless proven necessary.
- Charts: Recharts with `isAnimationActive={false}` everywhere; datasets above ~2K points
  go through LTTB downsampling in `src/lib/lttb.ts` before hitting Recharts. Chart
  components are pure functions of props and memoized.
- Tests: Vitest + Testing Library for components and `lib/`; Playwright for one smoke path.

## Benchmark result: JSON schema

One **result** = one trial of one metric for one (machine, workload, thread_count,
working_set_bytes) configuration. The engine emits a **run** document containing many
results; the API's ingest endpoint accepts the same document. The normative JSON Schema
will live at `schema/benchmark-result.schema.json`; this is the human-readable version.

### Result (one object)

```json
{
  "schema_version": 1,
  "run_id": "5b1f5e0a-2a1e-4c8b-9a1e-9d6a9a1b2c3d",
  "machine": {
    "id": "a3f1…",                          
    "hostname": "wsl-thinkpad",
    "cpu_model": "Intel(R) Core(TM) Ultra 9 185H",
    "physical_cores": 11,
    "logical_cpus": 22,
    "l1d_kb": 48,
    "l2_kb": 2048,
    "l3_kb": 24576,
    "memory_bytes": 16659374080,
    "os": "Ubuntu 22.04.5 LTS (WSL2)",
    "kernel": "6.18.33.2-microsoft-standard-WSL2",
    "compiler": "g++ 13.4.0",
    "compiler_flags": "-O3 -march=native",
    "engine_version": "0.1.0",
    "engine_git_sha": "abc1234"
  },
  "workload": "cpu_int",
  "thread_count": 8,
  "working_set_bytes": 0,
  "metric": "cpu_int_ops",
  "value": 12345678.9,
  "unit": "ops/s",
  "trial": 17,
  "timestamp": "2026-09-18T15:47:00.123456Z",
  "duration_ns": 50123456,
  "params": {
    "cold": "clflush",
    "pinned": true,
    "warmup_trials": 5,
    "trial_ms": 50
  }
}
```

Field rules:

| Field | Type | Rule |
|---|---|---|
| `schema_version` | integer | Currently `1`. |
| `run_id` | UUID v4 string | Same for every result of one `bench run` invocation. |
| `machine.id` | string (hex) | SHA-256 of `cpu_model|physical_cores|logical_cpus|l3_kb|memory_bytes|hostname`, truncated to 16 bytes (32 hex chars). Stable across runs on the same box; the API upserts machines by it. |
| `machine.*_kb` | integer | Per-core L1d and L2 (largest per-core value on hybrid parts), total L3. `0` if unknown. |
| `workload` | enum | `cpu_int`, `cpu_fp`, `cpu_hash`, `mem_bw`, `mem_latency`, `disk_seq`, `disk_rand`. |
| `thread_count` | integer ≥ 1 | Worker threads (for disk: concurrent I/O submitters = queue depth). |
| `working_set_bytes` | integer ≥ 0 | Bytes touched per thread (memory), file size (disk), or `0` if the workload has no memory working set (pure CPU). |
| `metric` | enum | One of the 12 below. |
| `value` | number | Finite. Higher-is-better or lower-is-better is a property of the metric, not the row. |
| `unit` | string | Fixed per metric (table below). Stored redundantly so rows are self-describing. |
| `trial` | integer ≥ 0 | 0-based index **after** warmup. Warmup trials are never emitted. |
| `timestamp` | RFC 3339 UTC, µs | When the trial finished. |
| `duration_ns` | integer | Measured wall time of the timed region for this trial. |
| `params` | object | Free-form but stable keys; everything needed to reproduce (cold mode, pinning, trial length, block size, queue depth, seed). |

### Run envelope (what the engine writes and the API ingests)

```json
{
  "schema_version": 1,
  "run_id": "…",
  "started_at": "…",
  "finished_at": "…",
  "machine": { "…same block as above…" },
  "argv": ["bench", "run", "--all", "--trials", "1000"],
  "results": [ { "…result without the machine block…" } ],
  "summary": [
    { "workload": "cpu_int", "metric": "cpu_int_ops", "thread_count": 8, "working_set_bytes": 0,
      "n": 1000, "mean": 1.2e7, "median": 1.2e7, "stddev": 2.1e5, "cov": 0.0175,
      "min": 1.1e7, "p5": 1.15e7, "p95": 1.25e7, "max": 1.3e7 }
  ]
}
```

Inside a run envelope, results omit `machine` (it is hoisted to the top level) to keep
1000-trial files small. The API flattens them back into full results on ingest.

## The 12 dashboard metrics

Each metric has an exact definition of what one "operation" or unit is. These definitions
are normative; `PLAN.md` describes how each is measured.

| # | `metric` | `workload` | `unit` | Better | Definition |
|---|---|---|---|---|---|
| 1 | `cpu_int_ops` | `cpu_int` | `ops/s` | higher | One op = one update of one 64-bit integer lane: `acc = (acc * K) + (acc >> 17) ^ i` (mul, shift, xor, add). 8 independent lanes per thread. Aggregate across threads. |
| 2 | `cpu_fp_ops` | `cpu_fp` | `ops/s` | higher | One op = one double-precision fused multiply-add `a = a * b + c` on one of 8 independent lanes per thread. |
| 3 | `cpu_hash_ops` | `cpu_hash` | `ops/s` | higher | One op = hashing one 64-byte block with a 64-bit xxHash-style mixer (input block read from an L1-resident 4 KiB buffer). |
| 4 | `mem_read_bw` | `mem_bw` | `GB/s` | higher | Bytes read per second summing a `std::span<const uint64_t>` of `working_set_bytes` per thread. 1 GB = 1e9 bytes. |
| 5 | `mem_write_bw` | `mem_bw` | `GB/s` | higher | Bytes written per second filling a buffer of `working_set_bytes` per thread (regular stores; non-temporal variant recorded in `params`). |
| 6 | `mem_copy_bw` | `mem_bw` | `GB/s` | higher | Bytes **copied** per second (`memcpy` src→dst, each `working_set_bytes`). Counts destination bytes once, not read+write. |
| 7 | `mem_latency` | `mem_latency` | `ns` | lower | Nanoseconds per dependent load in a random cyclic pointer chase over `working_set_bytes` (one pointer per 64-byte line, Sattolo permutation). Swept across working sets to expose L1/L2/L3/DRAM. |
| 8 | `disk_seq_read_bw` | `disk_seq` | `MB/s` | higher | Bytes/s reading a pre-filled file sequentially with 1 MiB `O_DIRECT` reads. 1 MB = 1e6 bytes. |
| 9 | `disk_seq_write_bw` | `disk_seq` | `MB/s` | higher | Bytes/s writing 1 MiB `O_DIRECT` blocks sequentially, `fdatasync` included in the timed region. |
| 10 | `disk_rand_read_iops` | `disk_rand` | `IOPS` | higher | Completed 4 KiB `O_DIRECT` `pread` calls per second at uniformly random 4 KiB-aligned offsets, queue depth = `thread_count`. |
| 11 | `disk_rand_write_iops` | `disk_rand` | `IOPS` | higher | Same as #10 with `pwrite` + `O_DSYNC`. |
| 12 | `disk_rand_read_p99_us` | `disk_rand` | `us` | lower | 99th percentile latency of individual 4 KiB random reads within one trial (per-call `steady_clock` timing, histogram with 1 µs buckets). |

Derived views computed by the API/dashboard, not stored as metrics: scaling efficiency
(`ops at N threads / (N × ops at 1 thread)`), machine-vs-machine ratio, CoV per config.

## Target metrics and where they are measured

| Target | Precise definition | Milestone |
|---|---|---|
| Engine ≥ 10M ops/s | Aggregate `cpu_*_ops` value (sum of all threads' ops ÷ trial wall time from barrier release to last thread's finish) at the machine's best thread count. | M2.4 |
| CoV ≤ 3% over ≥ 1000 trials | Sample stddev ÷ mean of `value` over ≥ 1000 post-warmup trials of one config, cold-cache mode on, no outlier trimming. | M2.5 (CPU), M3.3 (memory) |
| API ingests/queries ≥ 100K measurements | `SELECT count(*) FROM measurements ≥ 100000` after `cmd/seed`; every list/aggregate endpoint returns correct results over that set. | M5.2 |
| API ≥ 1000 req/s, p95 ≤ 50 ms | k6 `constant-arrival-rate` at 1000 iter/s for 60 s against the seeded DB, `http_req_duration p(95) < 50`, error rate < 0.1%. | M5.4 |
| Dashboard: 12 metrics, 10K+ points smoothly | All 12 metrics have a chart; the Trials page renders ≥ 10,000 points with initial commit ≤ 500 ms and no frame > 50 ms during hover/zoom (Chrome Performance panel). | M6.3 |

## Environment caveats (this machine)

- **WSL2 is a VM.** The 22 "CPUs" are Hyper-V vCPUs. `lscpu` reports 11 cores × 2
  threads, which hides the real hybrid layout (6 P-cores + 8 E-cores + 2 LP E-cores).
  Pinning a thread to a vCPU does not guarantee which physical core runs it. Record this;
  expect worse variance than bare metal, and expect uneven per-thread throughput.
- **Frequency.** Laptop part with turbo and thermal limits. Long runs drift. The engine
  does a 500 ms spin before the first trial and interleaves configs when sweeping.
- **Disk.** Root is ext4 on a VHDX. `O_DIRECT` bypasses the guest page cache only; the
  Windows host may still cache the VHDX. Results are "virtual disk" numbers and are
  labeled as such. Never benchmark under `/mnt/c` (9p, no `O_DIRECT`).
- **`drop_caches` needs root.** The engine does not rely on it; cold disk reads use
  `O_DIRECT` + `posix_fadvise(DONTNEED)`, which bypass the guest page cache. Host-side
  caching of the VHDX cannot be ruled out from inside WSL2, and the file size does not
  need to exceed RAM; the limitation is documented rather than worked around.
- **Docker Desktop networking** adds latency between WSL and published ports. For the k6
  target, run the API natively (or inside the compose network with k6 in a container) and
  record which.
