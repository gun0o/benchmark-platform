# PLAN.md — phased build plan

Principles:
- **Thin slice first.** Phase 1 gets one CPU number from the engine into Postgres and onto
  one chart. Every later phase widens an existing pipe rather than adding a new one.
- **Every milestone has a verification you actually run.** The "Verify" block is the
  definition of done. If verification produces numbers, they get committed under
  `docs/results/` with the command that produced them.
- **Targets are measured, not assumed.** Where a target is not met, the recorded number is
  the real one, with an explanation. That is a more credible portfolio artifact than a
  claim.

Milestone IDs are `M<phase>.<n>`. Each lists: Build / Verify / Pitfalls.

---

## Phase 0 — Toolchain and skeleton

### M0.1 Toolchain
**Build**
- Install Go 1.23+ (tarball; this machine has no passwordless sudo, so it lives in
  `~/.local/opt/go` with `~/.local/opt/go/bin`, `~/go/bin` and `~/.local/bin` added to
  PATH in `~/.bashrc` and `~/.profile`), `golangci-lint` and `golang-migrate` via
  `go install`, `k6` and `ninja` as GitHub release binaries in `~/.local/bin`,
  `clang-format` and `check-jsonschema` via `pip install --user`.
- `psql` and `redis-cli` are not installed natively; use
  `docker compose exec postgres psql …` and `docker compose exec redis redis-cli …`.
- Confirm `docker compose version` works from WSL (Docker Desktop integration).
- Rename the default branch: `git branch -M main`.
- Done 2026-09-18: Go 1.27.1, k6 2.2.0, golangci-lint 2.13.2, migrate (dev), ninja 1.11.1,
  clang-format 23.1.1, check-jsonschema 0.38.0, Compose v2.39.2.

**Verify**
```
g++ --version | head -1      # 13.x
cmake --version | head -1    # ≥ 3.22
go version                   # ≥ 1.23
k6 version
docker compose version
```

**Pitfalls**
- Ubuntu 22.04's apt Go is 1.18; do not use it. Use the official tarball.
- k6 from apt on WSL needs the Grafana GPG key step; snap k6 is not available in WSL.
- Non-login shells spawned by tools may not source `~/.bashrc`; scripts and the
  Makefile prepend the user-local paths themselves.
- **ninja must be < 1.12 with CMake 3.22.** ninja 1.12+ fails FetchContent sub-builds on
  re-configure ("manifest 'build.ninja' still dirty after 100 tries"); CMake fixed its side
  in 3.28.5/3.29.3. Pinned ninja 1.11.1 in `~/.local/bin`. (Ubuntu 24.04 CI runners ship
  1.11.1 from apt.)

### M0.2 Repo skeleton and compose baseline
**Build**
- `deploy/docker-compose.yml` with `postgres:16` and `redis:7` only, healthchecks, named
  volumes, ports 5432/6379 published to localhost.
- `deploy/.env.example` (`DATABASE_URL`, `REDIS_URL`, `API_ADDR`).
- `schema/benchmark-result.schema.json` (draft 2020-12) transcribed from `CLAUDE.md`,
  with `$defs` for `machine`, `result`, `run`, `summary`, and enums for `workload`,
  `metric`, `unit`.
- Root `Makefile` with `up`, `down`, `test`, `bench`, `seed`, `load` targets (most call
  into sub-projects; missing ones print "not yet").
- `.github/workflows/ci.yml` skeleton: three jobs (engine, api, dashboard), each just
  `echo skip` until the component exists.

**Verify**
- `docker compose -f deploy/docker-compose.yml up -d` → both services healthy in `docker compose ps`.
- `psql "$DATABASE_URL" -c 'select 1'` and `redis-cli ping` succeed from WSL.
- A hand-written sample run JSON validates against the schema:
  `check-jsonschema --schemafile schema/benchmark-result.schema.json sample.json`.
- A deliberately broken sample (missing `unit`) fails validation.

- Done 2026-09-18: postgres 16.15 and redis 7 healthy; `select 1` and `PING` OK;
  `schema/examples/run.json` validates and six deliberately broken variants are rejected.

**Pitfalls**
- Postgres healthcheck must use `pg_isready -U $POSTGRES_USER` or the API will race it.
- Keep the schema file as the single source of truth; the engine's writer, the Go
  model, and `openapi.yaml` all reference it (M1.3 adds a test that they agree).

---

## Phase 1 — Thin end-to-end slice (CPU benchmark → API → Postgres → one chart)

### M1.1 Engine skeleton
**Build**
- `engine/CMakeLists.txt` with a `bench` executable and a `bench_core` static library;
  `CMakePresets.json` with `debug`, `release` (`-O3 -march=native -fno-omit-frame-pointer`),
  `asan`, `tsan` presets. FetchContent: nlohmann/json, CLI11, GoogleTest.
- `sysinfo`: parse `/proc/cpuinfo`, `/sys/devices/system/cpu/cpu0/cache/index*/size`,
  `/proc/meminfo`, `uname`, `/etc/os-release`; detect WSL via `/proc/version` containing
  `microsoft`. Compute `machine.id` as specified in `CLAUDE.md`.
- `result.hpp`: `Result` struct and `RunEnvelope`, serialized with nlohmann/json.
- CLI: `bench sysinfo`, `bench list`, `bench run --workload cpu_int --threads 1 --trials N
  --out file.json`, `bench validate file.json` (structural check: required keys, enums).
- One placeholder workload `cpu_int` running on the calling thread, one trial, naive
  timing. It only needs to produce a well-formed document.

**Verify**
- `ctest` passes (sysinfo parsing tests with fixture `/proc` snapshots; JSON round-trip test).
- `bench sysinfo` shows the correct L3 (24576 KiB) and 22 logical CPUs on this machine.
- `bench run … --out run.json && check-jsonschema --schemafile schema/… run.json` passes.

**Pitfalls**
- `/sys/.../cache/index*` may be missing under WSL for some levels; fall back to `lscpu`
  parsing or `0`, never crash.
- `-march=native` binaries must not be shipped in Docker images; the engine-builder
  Dockerfile (M7.1) uses `-march=x86-64-v3`.

### M1.2 Measurement fundamentals (single thread)
This milestone is where benchmark correctness is established. Everything later reuses it.

**Build**
- `timing.hpp`:
  - `DoNotOptimize(T& v)`: `asm volatile("" : "+m,r"(v) : : "memory")` for register-sized
    trivially-copyable types, `"+m"` only for anything larger (Google Benchmark style; the
    `m` alternative must come first or GCC reports "impossible constraint" for constants).
    `ClobberMemory()`: `asm volatile("" ::: "memory")`.
  - `Timer` around `std::chrono::steady_clock`; `clock_resolution_ns()` via
    `clock_getres(CLOCK_MONOTONIC)`; a self-test that back-to-back `now()` calls differ
    by ≤ 100 ns on average (detects a bad clocksource).
- Trial loop in `runner`: `warmup_trials` (default 5, discarded) then `trials` timed
  trials; a global 500 ms busy spin before the first warmup so the core reaches turbo.
- Each trial is **time-boxed**: the workload runs in fixed-size batches (e.g. 1M ops)
  and checks the clock between batches until `trial_ms` (default 50 ms) has elapsed;
  `value = ops_done / elapsed_seconds`. The check-between-batches design keeps clock
  reads out of the inner loop.
- `cpu_int` kernel implemented for real: 8 independent `uint64_t` lanes, `K` and the
  initial lane values come from a runtime-seeded `std::mt19937_64` (so the compiler cannot
  constant-fold), final lanes are XOR-reduced into one value passed to `DoNotOptimize`.

**Verify**
- **Not optimized away:** `objdump -d --no-show-raw-insn build/release/bench | grep -A40 cpu_int` shows
  `imul`/`shr`/`xor`/`add` inside the loop. Also `bench run --workload cpu_int --trial-ms 50`
  vs `--trial-ms 100` reports ~2× `ops_done` for the same `ops/s` (±5%).
- **Debug vs release sanity:** debug build is 3–20× slower, not 1000× (that would mean
  release is skipping work).
- **Warmup matters:** log per-trial values with `--verbose`; trial 0 without warmup is
  visibly lower than trial 5+ (this is the justification for warmup, record it).
- Unit test: the timer self-test; a test that `DoNotOptimize` compiles for `int`,
  `double`, `uint64_t*`, and a 64-byte struct.

**Pitfalls**
- `high_resolution_clock` is an alias of `system_clock` on libstdc++ and can jump. Use
  `steady_clock` only.
- Reading the clock every iteration adds ~20 ns per read and serializes the pipeline;
  batch it.
- Auto-vectorization is allowed (it does not change how many source-level ops ran), but
  the lane count (8) must be enough to hide the 3–4 cycle `imul` latency, otherwise the
  benchmark measures latency rather than throughput. If `ops/s` ≈ core_GHz / 4, that is
  the symptom.
- `-ffast-math` would let the compiler reassociate the FP kernel (M2.4); it is banned.
- Done 2026-09-18: 36 tests green in release and debug. Trial-length scaling 1.969× ops at
  0.983× ops/s; debug 7.5× slower; trial 0 without warmup −7% (median of 3) to −55% below
  steady state, −0.2% with warmup. Full write-up in `docs/notes/M1.2.md`, runs in
  `docs/results/m1.2/`.
- **Finding:** on WSL2 the clock self-test is a host-contention canary. Every run whose
  mean `now()` exceeded 100 ns (115–151 ns vs 33 ns normally) also showed 2–3× lower
  throughput, with the guest 99% idle. Guest-side `taskset` did not help (vCPUs float on
  Hyper-V). M2.5 re-runs any config whose self-test failed.

### M1.3 API: ingest and list
**Build**
- `api/` module; `cmd/api` with `net/http` ServeMux; `slog`; config from env.
- Migrations `0001_init`: tables
  - `machines(id text pk, hostname, cpu_model, physical_cores, logical_cpus, l1d_kb, l2_kb, l3_kb, memory_bytes, os, kernel, compiler, compiler_flags, first_seen timestamptz, last_seen timestamptz)`
  - `runs(id uuid pk, machine_id text fk, engine_version, engine_git_sha, started_at, finished_at, argv jsonb, ingested_at)`
  - `measurements(id bigserial pk, run_id uuid fk, machine_id text fk, workload text, metric text, thread_count int, working_set_bytes bigint, trial int, value double precision, unit text, recorded_at timestamptz, duration_ns bigint, params jsonb)`
  - index `measurements(machine_id, metric, workload, thread_count, working_set_bytes)`; index `measurements(run_id)`.
- `POST /v1/runs`: accepts the run envelope; validates against the schema (embed
  `schema/benchmark-result.schema.json` via `go:embed` and use a JSON Schema library, or
  strict struct decoding + enum checks); upserts machine, inserts run, `CopyFrom` for
  measurements in one transaction. Returns `{run_id, inserted}`. Idempotent on `run_id`
  (409 or no-op on duplicate; pick no-op and return `inserted: 0`).
- `GET /v1/measurements?machine_id&workload&metric&thread_count&working_set_bytes&limit&cursor`
  with keyset pagination on `id`.
- `GET /healthz` (always 200), `GET /readyz` (pings PG).
- `openapi.yaml` describing these two endpoints.
- Tests: handler unit tests with an in-memory fake store; integration test (build tag)
  against compose Postgres that ingests a fixture run and reads it back.

**Verify**
- `go test ./... && go test -tags integration ./...` pass.
- `curl -X POST localhost:8080/v1/runs -d @engine/run.json` → `inserted: N`; second POST
  → `inserted: 0`; `psql -c 'select count(*) from measurements'` = N.
- A test asserts the Go model's enum lists equal the schema file's enums (parses the
  schema at test time), so the two cannot drift silently.

**Pitfalls**
- Do not insert row-by-row; `CopyFrom` is the difference between 100K rows in ~1 s and ~60 s.
- `timestamptz` everywhere; the engine emits UTC with `Z`.
- Keyset pagination, not `OFFSET`, or the 100K-row list endpoint will fall over in M5.4.

### M1.4 Engine posts to API
**Build**
- `bench run … --post URL` POSTs the envelope with a minimal libcurl-free HTTP client
  (a ~100-line blocking HTTP/1.1 client over sockets is enough; or shell out to `curl`
  if present — pick the socket client to avoid a runtime dependency) and prints the
  response. Also always writes `--out` if given.

**Verify**
- `bench run --workload cpu_int --threads 1 --trials 10 --post http://localhost:8080`
  → API logs the ingest, row count increases by 10.

**Pitfalls**
- Time-out the socket; a hung API must not hang the benchmark.

### M1.5 Dashboard: one chart
**Build**
- `npm create vite@latest` (react-ts), add Recharts, TanStack Query, ESLint/Prettier,
  Vitest. `openapi-typescript` generates `src/api/types.ts` from `../api/openapi.yaml`.
- One page: fetch `/v1/measurements?metric=cpu_int_ops`, group by `thread_count`,
  render a `LineChart` of median ops/s vs thread count for the current machine.
- Vite dev proxy `/v1` → `http://localhost:8080`.

**Verify**
- After running `bench run --threads 1,2,4,8` (still one thread of work at this point;
  the thread sweep becomes real in M2.1) the chart shows one line with 4 points and the
  numbers match `psql` medians.
- `npm run typecheck && npm run lint && npm run test` pass (one test: grouping helper).

**Pitfalls**
- Recharts animations mask render cost; `isAnimationActive={false}` from the start.
- Keep the median computation in a pure `lib/` function so it is unit-testable and
  later replaceable by the API's aggregate endpoint (M5.3).

### M1.6 Compose runs the whole slice
**Build**
- Multi-stage Dockerfiles for `api` (distroless) and `dashboard` (nginx serving `dist/`,
  proxying `/v1` to `api:8080`). Add both to `docker-compose.yml`. `api` waits on PG
  healthcheck and runs migrations at start (`cmd/migrate` or a `migrate` init service).

**Verify**
- `docker compose up -d --build` → `http://localhost:3000` shows the chart populated from
  a native `bench run --post http://localhost:8080`.
- `docker compose down -v && up` → empty chart, no crash (empty-state handled).

**Pitfalls**
- nginx proxy must forward to the compose service name, while the Vite dev proxy uses
  localhost; keep both configs and document which is which.

**Phase 1 exit criterion:** one real CPU measurement travels engine → API → Postgres →
browser with zero manual steps beyond `make up` and `make bench`.

---

## Phase 2 — Engine: multithreading, statistics, cold cache, CPU metrics

### M2.1 Worker pool with synchronized start
**Build**
- `runner`: for `thread_count = N`, spawn N `std::jthread`s. Each worker:
  1. Pins itself with `pthread_setaffinity_np` to CPU `cpu_list[i]` when `--pin` is set
     (default list: `0..N-1`; `--cpus 0,2,4,…` overrides; topology from `sysinfo`).
  2. Allocates and first-touches its own buffers (so pages are local to the thread).
  3. For each trial: performs the cold-cache prep (M2.3) → `barrier.arrive_and_wait()`
     → runs the time-boxed kernel → records `ops_done` and its own start/end
     `steady_clock` stamps into its padded slot → `latch.count_down()` (or a second
     barrier phase).
  4. Checks `stop_token` between trials so Ctrl-C exits cleanly.
- The main thread records `t_release` right before the barrier completes (using the
  barrier's completion function, which runs in exactly one thread at phase completion)
  and `t_done` when the latch/second phase completes. **Aggregate throughput for the
  trial = Σ ops_done / (t_done − t_release).** Per-thread throughput is also recorded in
  `params.per_thread` for diagnostics.
- Per-thread result slot: `struct alignas(128) Slot { uint64_t ops; ns start; ns end; }`
  in a `std::vector<Slot>` (the alignment applies to elements; verify with `static_assert(sizeof(Slot) == 128)`).
- `Workload` concept: `{ w.setup(ctx) } ; { w.run_batch(ctx) } -> std::same_as<uint64_t>`
  (returns ops done); `{ w.teardown() }`. Workloads are stateless value types; the
  runner creates one per thread.

**Verify**
- **Start synchronization:** with `--verbose`, per-thread `start` stamps for a trial
  span < 50 µs (report the max spread over 100 trials). Without the barrier (temporary
  `--no-barrier` flag, then remove it) spread is 100s of µs to ms: record both numbers.
- **False sharing A/B:** a test binary runs the counter loop with `Slot` padded vs
  packed (`alignas(8)`) on 8 threads; packed must be measurably slower (expect ≥ 2×).
  Keep this as `tests/false_sharing_test` with a loose assertion (padded ≥ 1.3× packed).
- **Pinning:** `--pin --verbose` prints `sched_getcpu()` from each worker; each equals
  its assigned CPU throughout the trial (sample at start and end).
- `tsan` preset run of the pool is clean.
- Scaling: `cpu_int` at 1,2,4,8 threads is ~linear up to the number of "real" cores;
  record the curve, it is the first real dataset.

**Pitfalls**
- `std::barrier`'s completion function is the right place to grab `t_release`; do not
  have thread 0 read the clock after the barrier (it may be descheduled).
- Hyper-V may migrate vCPUs regardless of guest pinning; the verification only proves
  guest-level pinning. Say so in `docs/methodology.md`.
- `alignas(128)` on a struct inside `std::vector` works, but `std::vector<Slot>` requires
  the allocator to honor over-alignment (C++17+ does). Add the `static_assert`.
- 22 vCPUs include hyperthreads; pinning threads 0 and 1 may land on siblings. Under WSL2
  sibling info is synthetic, so the default `--cpus` list uses stride 2 first
  (0,2,4,…) then fills odds; record the actual list in `params.cpus`.
- Workers must not allocate in the timed region.
- Done 2026-09-18 (write-up: `docs/notes/M2.1.md`, runs: `docs/results/m2.1/`). Deviations
  from the text above, all measured:
  - **`std::barrier` replaced by `bench::SpinBarrier`** (`engine/include/bench/sync.hpp`).
    `std::barrier` parks waiters on a futex; on WSL2 that gave start spreads with medians
    of 85-1100 us and maxima of 3-10 ms for 8 threads, and a spin-then-`wait(token)`
    hybrid still hit ms tails via libstdc++'s yield-then-futex path. The spin barrier
    (arrival counter + generation, completion function in the last arriver) gives a
    0.3-0.5 us median. `std::latch` is used for "all workers finished setup".
  - **Workers never sleep between trials.** Sleeping at the end barrier parks the vCPU and
    the next release pays the un-park latency; both barriers spin.
  - **Per-configuration warmup has a time floor (`--warmup-ms`, default 500).** The first
    ~250 ms after spawning a pool show multi-ms spreads while Hyper-V settles the busy
    vCPUs; trial-count warmup alone (5 x 20 ms) did not cover it.
  - The `--no-barrier` flag was not added; the A/B lives in `tests/barrier_sync_test.cpp`
    (none vs `std::barrier` vs `SpinBarrier`, warmed up like the runner).
  - Start spread target "< 50 us": met by the typical trial (median 0.33 us pinned,
    0.51 us unpinned; 91-93 of 100 trials under 50 us) but not by every trial (max
    0.8-2 ms). The residue is host-level vCPU preemption and is recorded per trial in
    `params.start_spread_us`.
  - Pinning: 0 violations, but at 22 threads pinning is worse than not pinning (0.33 vs
    0.43 scaling efficiency): 22 spinning workers plus the polling main thread on 22 vCPUs
    is oversubscribed. Default `--pin` guidance: use it up to ~half the vCPUs.
  - Scaling (median of 20 x 50 ms trials): 4.1 Gops/s at 1 thread, near-linear to 4
    threads (16.7), 27.4 at 8, 32.9 at 16, ~30-34 at 22; per-thread throughput halves
    beyond 8 threads (P-core vs E-core vCPUs, hyperthreads). First real dataset.

### M2.2 Statistics
**Build**
- `stats.hpp`: `Welford` (online mean/variance, merges), `median`, `percentile(p)`
  (linear interpolation, same as numpy default), `cov = sample_stddev / mean`, `min`,
  `max`, `mad`. Implemented over `std::span<const double>` / ranges.
- Runner emits every trial as a result **and** a `summary` entry per config
  (`n, mean, median, stddev, cov, min, p5, p95, max`).
- `--trials N --trial-ms T` plus `--max-seconds` safety cap.

**Verify**
- Unit tests against committed numpy fixtures (`tests/fixtures/stats.json`: 10 random
  arrays and their `mean/std(ddof=1)/median/p5/p95`), tolerance 1e-9.
- Welford on `[1e9+1, 1e9+2, 1e9+3]` gives variance 1.0 (catastrophic-cancellation test).
- Merging two Welford halves equals one pass over the concatenation.

**Pitfalls**
- Sample (n−1) vs population stddev: use sample; state it in the summary.
- Percentile definitions differ across tools; pin to numpy `linear` and say so.
- Done 2026-09-18 (write-up: `docs/notes/M2.2.md`, runs: `docs/results/m2.2/`). Notes on
  what the measurements forced:
  - **Welford is not used for the batch statistics.** It met the cancellation and merge
    checks, but on the ill-conditioned fixture (values ~1e9, stddev ~1) its single pass
    lands 1.4e-8 from numpy, which fails the 1e-9 tolerance this milestone asks for. The
    batch path (`mean`, `sample_variance`, `sample_stddev`, `cov`, `summarize`) is the
    two-pass algorithm numpy itself uses and matches to 1.5e-14. `Welford` stays in
    `stats.hpp` with its merge for the streaming case (per-thread accumulators, M2.5+),
    documented as accurate to ~kappa·eps where kappa = mean/stddev.
  - The Welford merge test's tolerance scales with that condition number rather than
    being a flat 1e-9: reassociating floating-point work cannot do better.
  - `mad` was added to the summary (optional in the schema, so no `schema_version` bump)
    and is worth having: on the thread sweep, stddev/MAD ran 1.5–2.8 where clean Gaussian
    noise would give 1.48, which is how a few slow trials announce themselves.
  - `--max-seconds` stops at a trial boundary and still emits a summary over the trials
    that ran; `params.warmups_run` and `summary.n` say what actually happened.
  - Three pre-existing test problems surfaced and were fixed: the clock self-test asserted
    a ~30 ns `now()` cost that no sanitizer build can meet (now skipped under ASan/TSan
    with the reason printed); the trial-length scaling test compared against a literal 2x
    instead of the measured rate, folding in up to one batch of overshoot; and that same
    test used 3-trial means, whose standard error on a machine with ~10% per-trial
    variation made any tight bound flaky (now 9 trials compared by median).
  - The `asan` preset is now part of the routine check: 55/55 in release, debug, asan and
    tsan.

### M2.3 Cold-cache infrastructure
**Build**
- `cache.hpp`:
  - `flush_lines(std::span<const std::byte>)`: `_mm_clflushopt` every 64 B, then
    `_mm_sfence()`; falls back to `_mm_clflush` if `clflushopt` is not in CPUID
    (checked once via `__builtin_cpu_supports`/cpuid). Followed by `_mm_mfence()` before
    the barrier.
  - `evict_llc()`: per-thread streaming read over a private buffer of `2 × L3` bytes
    (48 MiB here) with a data dependency into `DoNotOptimize`, for machines/VMs where
    `clflushopt` is unavailable or when the working set is huge.
  - `alloc_buffer(bytes)`: `posix_memalign(4096)` or `mmap` + `madvise(MADV_HUGEPAGE)`
    for ≥ 2 MiB, first-touch by the owning thread. Records whether THP was granted
    (`/proc/self/smaps_rollup` AnonHugePages delta) into `params.huge_pages`.
- `--cold {clflush|evict|none}` (default `clflush`). Cold prep happens **before** the
  barrier so it is outside the timed region, and every thread does it for its own buffers.
  For pure-CPU workloads the "working set" is the 4 KiB input block (`cpu_hash`) or
  nothing (`cpu_int/fp`); cold mode still flushes the input block so all trials start
  from the same state.

**Verify**
- **Flush works:** test allocates 256 KiB (fits L2), pointer-chases it (warm: ~4 ns/load
  on this part), calls `flush_lines`, chases once more: first pass after flush must be
  ≥ 10× slower per load (DRAM ~80–100 ns). Assert ≥ 5× to tolerate VM noise.
- **Eviction works:** same test with `evict_llc` instead of flush; expect ≥ 3× slower.
- `--cold none` vs `--cold clflush` on `mem_latency` at 1 MiB working set: cold shows the
  first-batch penalty, steady-state values converge (record).

**Pitfalls**
- `clflushopt` needs `-mclflushopt` or `-march=native`; guard with `#ifdef __CLFLUSHOPT__`.
- Flushing 512 MiB line-by-line takes ~100 ms; for working sets > L3 skip the flush
  (the buffer cannot be cache-resident anyway) and record `params.cold = "n/a"`.
- Prefetchers can refill lines between flush and barrier if the thread touches nearby
  memory; do the flush last, then only the barrier.
- Done 2026-09-19 (write-up: `docs/notes/M2.3.md`, runs: `docs/results/m2.3/`). Measured
  plugged in, Windows plan Balanced, power mode **Best power efficiency** (left as found,
  so absolute values are a floor). Deviations from the text above, all measured:
  - **Runtime CPUID dispatch instead of `#ifdef __CLFLUSHOPT__`.** The flush loop carries
    `[[gnu::target("clflushopt")]]` and is selected by `__builtin_cpu_supports`, so the
    portable `ci` build (`-march=x86-64-v3`, which excludes CLFLUSHOPT) still emits the
    instruction and reaches it: 14 sites, same 29x ratio as the native build. An `#ifdef`
    would have silently demoted `ci` to `CLFLUSH` on a CPU that supports the fast one.
  - **THP is read per-mapping from `/proc/self/smaps`, not as a `smaps_rollup` delta.** The
    rollup is process-wide and several workers allocate concurrently, so a before/after
    difference can credit one thread's huge pages to another. (The first parser classified
    every mapping header as a field line, because the header's device field `08:40` also
    contains a colon, and reported a plausible 0 huge pages for a buffer that had 100%.)
  - **The 2 MiB alignment matters less than assumed.** Measured on a 48 MiB mapping:
    aligned+madvise 48/48 MiB, aligned without madvise 0, unaligned+madvise 46/48. Under
    the `madvise` policy the madvise call does the work; alignment buys back the partial
    huge page at each end. Both are done; the header comment was corrected to match.
  - **`params.cold` records what happened, `params.cold_requested` what was asked.** A
    region larger than L3 (or of zero bytes) gives `cold: "n/a"`. Flushing dirty lines
    measured ~5.5 ns/line, so 512 MiB would cost ~46 ms/trial, confirming the pitfall.
  - **Verify 3 ran on the pointer chase, not `mem_latency`** (M3.2). Steady state is pass
    3, not pass 2: at 1 MiB the second pass after a flush is still ~15% slow. First pass
    22-36x warm, converged by p3 at all of 64 KiB / 256 KiB / 1 MiB.
  - **Verify 2 needed 41 repetitions, not 9.** At 9 the eviction ratio swung 2.95x-23.63x
    on the same binary and failed its own 3x bar once. At 41, ten consecutive runs gave
    9.4x-25.8x (median 16.4x) against clflush's 13.5x-31.1x (median 28.7x). clflush cools
    more thoroughly (higher post-cool latency in 8 of 10 runs) and costs 0.9 us against
    eviction's 2.4 ms, so it stays the default.
  - **Pinning the latency test to vCPU 0 made it worse**, not better (warm baseline 3.65 ->
    12.34 ns/load in one run of three): vCPU 0 carries interrupt work, and guest-level
    pinning does not determine the physical core (M2.1). Left unpinned; repetitions are the
    fix for the spread.
  - Cold mode leaves `cpu_int` unchanged (<2% across all three modes, inside a 6-20% CoV),
    which is the expected result for an 80-byte working set and is the evidence that
    preparation stays outside the timed region.
  - `params.cold_requested`, `cold_bytes`, `cold_prep_{mean,max}_us` and `huge_page_bytes`
    were added to the schema alongside the existing `cold` and `huge_pages` (additive, no
    `schema_version` bump), and `bench validate` now checks the `params` keys the schema
    pins down.
  - 20 new tests (16 functional + 4 perf-labelled); 71/71 in release, debug, asan, tsan and
    ci, 7/7 perf in release.

### M2.4 CPU workloads and the 10M ops/s target
**Build**
- `cpu_int` (from M1.2), `cpu_fp` (8 lanes of `std::fma(a, b, c)` with runtime `b, c`;
  8 lanes hide the 4-cycle FMA latency; a lane is a `double`), `cpu_hash` (64-byte block
  hash: read 8×`uint64_t` from an L1-resident 4 KiB buffer at a rotating offset, mix with
  xxHash64-style multiply-rotate-xor rounds using `std::rotl` and `std::bit_cast`,
  fold into one accumulator).
- Op counts are computed from batch size × lanes, never estimated.
- `bench run --workload cpu_int,cpu_fp,cpu_hash --threads 1,2,4,8,11,16,22`.

**Verify**
- Record `docs/results/cpu_<date>.json` and a table in `docs/results/README.md`:
  per workload, best thread count, aggregate ops/s, per-thread ops/s at 1 thread.
- **Target #1 check:** aggregate `cpu_*_ops` ≥ 1e7 at best thread count. (Expected to be
  exceeded by orders of magnitude for `cpu_int/fp`, since one op is a few instructions;
  the definition is what makes the number meaningful. Also record ops/s/thread and
  ops per cycle estimate = ops/s ÷ nominal GHz, which is the interesting number.)
- `objdump` check per kernel (FMA: `vfmadd` present; hash: `imul` + `rol`).
- Iteration-scaling check from M1.2 applied to all three.

**Pitfalls**
- FP: without `fma` intrinsics or `-mfma`, GCC emits separate mul+add and the op
  definition becomes wrong; check the disassembly.
- The hash input buffer must be > lane state but ≤ L1d (4 KiB) so the workload is
  compute-bound, not L1-bandwidth-bound.
- Hybrid cores: per-thread throughput on E-core vCPUs is ~40–60% of P-core; aggregate
  scaling flattens after ~6 threads. This is a real finding, plot it.

### M2.5 Variance: CoV ≤ 3% over 1000 trials
**Build**
- `--trials 1000 --trial-ms 50 --warmup 20 --pin --cold clflush` for `cpu_int`,
  `cpu_fp`, `cpu_hash` at 1 thread and at best thread count (≈ 50 s per config).
- `--interleave`: when running multiple configs, rotate through them trial-by-trial
  instead of finishing one config before the next, so slow drifts (thermal) affect all
  configs equally. Off by default; used for the variance study.
- `bench report run.json` prints the summary table with CoV per config; the dashboard
  Trials page (M6.2) shows the same.

**Verify**
- Record CoV for each config in `docs/results/variance_<date>.md` along with:
  Windows power plan, whether plugged in, background load (`uptime` load average
  before/after), and ambient conditions.
- **Target #2 check:** CoV ≤ 0.03 for the pure-CPU configs. If not met on WSL2, run the
  same binary on bare-metal Linux if available and record both; otherwise document the
  achieved CoV and the dominant noise source (sample per-trial values, look for
  bimodality = core migration, slow drift = thermal, spikes = interrupts).
- Show the effect of each knob: CoV with/without `--pin`, with/without warmup, with
  `--trial-ms 10` vs `50` vs `200` (longer trials average out jitter; there is a
  trade-off with total runtime). This table is a key portfolio artifact.

**Pitfalls**
- Trial 0 after a config switch is warm-vs-cold for turbo state; the 20 warmup trials
  absorb it.
- Do not trim outliers to hit the target. Report raw CoV; optionally also MAD/median as
  a robust companion, clearly labeled.
- Trials whose `start_spread_us` is large (say > 1 ms) had a late worker; that is host
  preemption, not the workload. Report the fraction of such trials per config alongside
  CoV; do not silently drop them.
- Use the clock self-test as a host-contention canary (M1.2 finding): if mean `now()`
  exceeds 100 ns at the start of a config, re-run that config rather than averaging in
  contaminated trials. Record how many re-runs were needed; that count is itself a
  measure of host noise. This is a pre-declared rule, not post-hoc trimming.
- Laptop on battery or "Balanced" power plan can swing 20%; the run notes must say
  which plan was active.
- Postgres/Redis containers idle at ~0% CPU but Docker Desktop's VM does not; for the
  variance study stop compose (`make down`) and write results to a file, then post later.

---

## Phase 3 — Memory bandwidth and cache latency

### M3.1 Bandwidth (`mem_read_bw`, `mem_write_bw`, `mem_copy_bw`)
**Build**
- Per-thread buffers of `working_set_bytes` (`--working-set 4K,32K,256K,1M,8M,32M,256M`),
  huge pages when ≥ 2 MiB, first-touched by the owner thread.
- Read: 8-lane `uint64_t` sum over `std::span<const uint64_t>` (lanes so it vectorizes
  and is not add-latency-bound); write: fill with a runtime value; copy: `std::memcpy`.
  Optional `--nt` uses `_mm256_stream_si256` for write/copy and is recorded in `params`.
- Bytes are counted from the span size, and `value = bytes / elapsed / 1e9`.
- Cold mode: for working sets ≤ L3, flush before the barrier (M2.3); for larger, `n/a`.

**Verify**
- Working-set sweep at 1 thread shows plateaus: L1 (~100+ GB/s), L2, L3, DRAM
  (~30–60 GB/s single-thread DDR5). Thread sweep at 256 MiB saturates DRAM bandwidth and
  flattens; record the knee.
- Write ≈ ½ read bandwidth in DRAM regime without `--nt` (write-allocate reads the line
  first); with `--nt` write approaches read. Explain this in `docs/methodology.md`.
- `objdump`: the read loop is vectorized (`vpaddq` on ymm).

**Pitfalls**
- `std::fill` with a constant may become `memset` and the "op" definition shifts; that
  is acceptable for write bandwidth (memset is what programs use) but say so.
- Copy bandwidth must count destination bytes once; some tools count 2×.
- Small working sets at 50 ms trials mean millions of passes; batch = one full pass.
- Page faults on first touch would dominate if buffers were allocated in the timed region;
  they are not (M2.1 rule).

### M3.2 Latency (`mem_latency`)
**Build**
- Buffer of `working_set_bytes` viewed as an array of 64-byte lines; build a single cycle
  through all lines with Sattolo's algorithm (uniform random cyclic permutation, so the
  chase visits every line exactly once per lap and the prefetcher cannot predict it).
  Each line stores the index of the next line in its first 8 bytes.
- Kernel: `idx = buf[idx]` repeated; one op = one dependent load; batch = 1M loads;
  `value = elapsed_ns / loads`.
- Sweep `--working-set 4K,16K,32K,48K,64K,128K,512K,1M,2M,4M,8M,16M,24M,32M,64M,256M,1G`.
- Huge pages on for ≥ 2 MiB (records `params.huge_pages`) so TLB misses do not masquerade
  as cache misses; an explicit `--no-hugepages` run shows the TLB effect.

**Verify**
- 1-thread sweep shows steps near 48 KiB (L1d), 2 MiB (L2), 24 MiB (L3), then DRAM
  (~90–110 ns on this laptop). Plot it: this is the dashboard's signature chart.
- Trials with `--cold clflush` at 1 MiB: CoV recorded (Target #2 for memory, part of M3.3).
- With `--no-hugepages` the ≥ 64 MiB points rise noticeably (TLB), proving the huge-page
  path matters.

**Pitfalls**
- Sattolo, not Fisher–Yates: Fisher–Yates can create short cycles that keep the chase
  in a small cache-resident subset.
- Multi-thread latency (`--threads N`) measures loaded latency; label it as such.
- WSL2's THP setting may be `madvise` or `never`; the engine reads
  `/sys/kernel/mm/transparent_hugepage/enabled` and records it.

### M3.3 Variance for memory metrics
**Build**
- 1000-trial runs of `mem_latency` at 32 KiB / 1 MiB / 16 MiB / 256 MiB and
  `mem_read_bw` at 256 KiB / 256 MiB, cold vs none.

**Verify**
- CoV table added to `docs/results/variance_<date>.md`. Expect cold mode to **reduce**
  CoV for ≤ L3 working sets (every trial starts from the same state instead of inheriting
  the previous trial's residue), and to be irrelevant for > L3.

**Pitfalls**
- Flush time for 16 MiB (~260K lines) is a few ms; fine outside the timed region.

---

## Phase 4 — Disk I/O

### M4.1 Sequential and random I/O with O_DIRECT
**Build**
- Test file `--disk-path` (default `$HOME/.cache/bench/testfile`, must be on ext4;
  the engine refuses paths under `/mnt/` unless `--i-know-this-is-9p`). Size
  `working_set_bytes` (default 4 GiB). Cold reads rely on `O_DIRECT` plus
  `posix_fadvise(DONTNEED)`, which bypass the guest page cache; the file size does not
  need to exceed RAM for that. Host-side caching of the VHDX by Windows cannot be
  controlled or ruled out from inside WSL2, so disk results are labeled "virtual disk"
  and the run notes record `MemAvailable` in the guest and the file size. Created with
  `fallocate` then **filled with random data** in 1 MiB `O_DIRECT` writes + `fdatasync`
  (never leave it sparse or zero-filled; some storage stacks short-circuit zeros).
- Buffers via `posix_memalign(4096, size)`; file opened `O_RDONLY|O_DIRECT` (reads) or
  `O_WRONLY|O_DIRECT|O_DSYNC` (random writes); `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`
  before each trial (belt and braces alongside `O_DIRECT`).
- `disk_seq`: 1 MiB `pread`/`pwrite` sweeping the file; one thread per file region;
  `fdatasync` at trial end inside the timed region for writes.
- `disk_rand`: each of `thread_count` threads issues 4 KiB `pread` at random 4 KiB-aligned
  offsets (`std::mt19937_64` seeded from `params.seed`), queue depth = thread_count
  (one outstanding I/O per thread, synchronous). Per-call latency via `steady_clock`
  into a 1 µs-bucket histogram (bounded to 1 s); `disk_rand_read_iops` = calls / elapsed,
  `disk_rand_read_p99_us` from the histogram. `disk_rand_write_iops` likewise with
  `pwrite` + `O_DSYNC`.
- Trials are longer here (`--trial-ms 500` default for disk) because a 4 KiB read is
  ~100 µs and 50 ms would be too few samples for a p99.

**Verify**
- `strace -e trace=openat,pread64 -c` confirms `O_DIRECT` is set and reads are 4096/1 MiB.
- Sequential read on this VHDX: record MB/s at 1 and 4 threads; random read IOPS at
  1, 4, 16 threads; p99 at each. Compare with `fio --direct=1 --ioengine=psync` with the
  same parameters (fio is the reference tool; agreement within ~10% validates the
  engine). Commit both outputs.
- Cold check: a second immediate `disk_seq_read` trial is not faster than the first
  (guest page cache bypassed). Host-side caching cannot be verified or excluded from
  inside WSL; state this in `docs/methodology.md` and in the results table.

**Pitfalls**
- `O_DIRECT` requires buffer, offset, and length aligned to the logical block size
  (512 or 4096); `EINVAL` otherwise. Query with `ioctl(BLKSSZGET)` on the block device
  or just use 4096.
- `/mnt/c` (9p/drvfs) silently ignores or fails `O_DIRECT`; refuse it.
- Windows Defender real-time scanning inside the VHDX is not a factor for ext4, but
  Windows-side disk activity (indexing, updates) is; note the run conditions.
- Random **write** tests wear SSDs and can be slow with `O_DSYNC`; default the total
  written per run to ≤ 1 GiB and require `--allow-writes`.
- Delete the test file with `bench disk clean`; never leave 4 GiB in the repo.

---

## Phase 5 — API: scale, aggregation, cache, load

### M5.1 Ingest and query hardening
**Build**
- Ingest validates size (≤ 50K results per POST; larger runs are chunked by the engine),
  streams the JSON decode, `CopyFrom` in batches of 10K rows inside one transaction.
- `GET /v1/machines`, `GET /v1/machines/{id}`, `GET /v1/runs?machine_id`,
  `GET /v1/runs/{id}`.
- Request logging with duration, `X-Request-ID`, panic recovery, gzip for responses,
  timeouts (`ReadHeaderTimeout`, handler `context` deadline 5 s).
- `pgxpool` sized from env (`PG_MAX_CONNS`, default 20). Postgres tuned in compose:
  `shared_buffers=512MB`, `work_mem=32MB`, `synchronous_commit=off` **for the load-test
  profile only** (documented; default profile keeps it on).

**Verify**
- Ingest a synthetic 50K-result envelope in < 2 s (`time curl …`).
- `EXPLAIN (ANALYZE, BUFFERS)` for the list query with all filters uses the composite
  index (Index Scan, not Seq Scan).

**Pitfalls**
- `json.Decoder` on a 50 MB body allocates heavily; stream with `Token()` over
  `results` array. Measure allocations with `go test -bench . -benchmem`.

### M5.2 Synthetic data generator (100K+ measurements)
**Build**
- `cmd/seed --machines 5 --trials 100 --seed 42 --out seed.json | --post URL`.
  Generates: 5 fictional machines with distinct core counts / cache sizes / bandwidth
  parameters; for each: 12 metrics × thread counts `{1,2,4,8,16}` × working sets
  (4 per memory metric, 1 for CPU/disk) × `trials`. Sized so the default is ≥ 100K rows
  (5 × 12 × 5 × ~3.5 avg × 100 ≈ 105K; the command prints the exact count and refuses to
  exit 0 if < 100K unless `--allow-small`).
- Values come from a **model**, not uniform noise: CPU throughput follows
  `T(n) = T1 × n / (1 + (n−1)×s)` (Amdahl-like with contention `s`) with log-normal
  per-trial noise (σ chosen per machine, 1–4%); latency follows a step function over the
  machine's cache sizes with smoothing; bandwidth plateaus; disk IOPS grows with queue
  depth to a cap; p99 latency is skewed (log-normal, heavier tail). Timestamps spread
  over 30 days. Deterministic under the same seed.
- Same envelope format as the engine, so it goes through the real ingest path.

**Verify**
- `go run ./cmd/seed --post http://localhost:8080` then
  `psql -c 'select count(*) from measurements'` ≥ 100,000; total time recorded.
- Spot checks: `select metric, count(*) group by metric` has all 12; CoV per config
  computed in SQL (`stddev_samp(value)/avg(value)`) matches the generator's σ within 20%.
- `GET /v1/measurements?…` with each filter returns the expected counts (test asserts
  against SQL).
- A unit test runs the generator twice with the same seed and diffs the output.

**Pitfalls**
- Keep synthetic machines obviously synthetic (`machine.hostname = "synthetic-01"`,
  `engine_version = "seed"`) so they can be filtered out of real result pages; the
  dashboard shows a badge.
- Do not commit `seed.json` (it is ~100 MB); it is regenerated deterministically.

### M5.3 Aggregation, compare, Redis cache
**Build**
- `GET /v1/aggregates?machine_id&metric&workload&group_by=thread_count|working_set_bytes`
  → per group: `n, mean, median, stddev, cov, min, p5, p95, max` via SQL
  (`percentile_cont`, `stddev_samp`).
- `GET /v1/compare?machines=a,b,c&metric=…&workload=…` → aligned series per machine plus
  ratio to the first machine.
- `GET /v1/trials?machine_id&metric&workload&thread_count&working_set_bytes&limit=10000`
  → `(trial, value, recorded_at)` triples, compact array-of-arrays JSON, for the
  10K-point dashboard page. Optional `&downsample=lttb:2000` server-side.
- Redis cache-aside for aggregates/compare/trials: key = canonical query string,
  TTL 60 s, `DEL` by machine-prefixed key set on ingest for that machine. Circuit: on
  Redis error, log once per minute and serve from Postgres.
- `openapi.yaml` updated; dashboard types regenerated.

**Verify**
- Aggregates match a numpy computation over the same rows pulled via `/v1/measurements`
  (integration test, tolerance 1e-6 relative).
- Cache hit path p50 < 2 ms, miss path recorded; `redis-cli monitor` shows one `SET`
  per distinct query and `GET`s thereafter; ingest for machine X evicts X's keys only.
- `docker compose stop redis` → endpoints still work (slower), `/readyz` still 200 with
  `"redis": "degraded"`.

**Pitfalls**
- `percentile_cont` over 1000 rows × many groups is fine; over 100K rows unfiltered it is
  not; require `machine_id` and `metric` on aggregate endpoints (400 otherwise).
- Cache keys must include every filter, sorted, or two queries collide.

### M5.4 k6 load test: ≥ 1000 req/s at p95 ≤ 50 ms
**Build**
- `api/loadtest/read_heavy.js`: `constant-arrival-rate` executor, `rate: 1000`,
  `timeUnit: '1s'`, `duration: '60s'`, `preAllocatedVUs: 100`, `maxVUs: 400`.
  Request mix drawn from a pre-fetched list of real (machine, metric, workload) combos:
  60% `/v1/aggregates`, 20% `/v1/compare`, 15% `/v1/measurements` (paged), 5% `/v1/trials?limit=1000`.
  Thresholds: `http_req_duration: ['p(95)<50']`, `http_req_failed: ['rate<0.001']`,
  `dropped_iterations: ['count==0']` (proves the 1000/s rate was actually achieved).
- `api/loadtest/ingest.js`: 50 req/s of 100-result envelopes, for the mixed profile.
- `api/loadtest/mixed.js`: both scenarios concurrently.
- `make load` runs the read-heavy profile and writes `docs/results/k6_<date>.json`
  (`--summary-export`) plus the api's `/metrics` snapshot.
- Run matrix: (a) API native in WSL + PG/Redis in compose, k6 native; (b) everything in
  compose, k6 in a container on the compose network. Record both; (a) is the headline
  unless (b) also passes.

**Verify**
- **Target #4 check:** k6 summary shows `p(95) < 50 ms`, `http_reqs ≥ 60,000` in 60 s,
  `dropped_iterations = 0`, error rate < 0.1%. Commit the JSON and a table.
- Also record: with Redis disabled (to show its contribution), and at 2000 and 3000 rps
  to find the knee.
- CPU usage of api/k6/postgres during the run (`docker stats`, `top`) is noted; k6 on
  the same 22 vCPUs steals cycles, so the numbers are conservative.

**Pitfalls**
- `constant-vus` measures nothing useful for a throughput target; use arrival-rate.
- Cold cache at t=0 skews p95; add a 10 s ramp scenario or exclude the first 10 s via
  `startTime` on the measured scenario.
- Go's default `http.Server` has no limits; set `MaxHeaderBytes`, timeouts, and
  `GOMAXPROCS` (container-aware via `automaxprocs` if in compose with CPU limits).
- `pgxpool` too small → queueing shows up as p95; too large → Postgres context switching.
  Sweep 10/20/40 and record.
- Docker Desktop's port publishing on WSL2 goes through a proxy; the compose-network
  variant avoids it.

### M5.5 Observability
**Build**
- Prometheus metrics: request duration histogram by route/status, ingest rows counter,
  cache hit/miss counters, pgxpool stats. `pprof` on a separate admin port.
- Optional compose profile `observability` with Prometheus + Grafana and a provisioned
  dashboard (nice for the portfolio, not required).

**Verify**
- During `make load`, a CPU profile (`go tool pprof -http`) is captured and the top-3
  hot spots are listed in `docs/results/k6_<date>.md`.

---

## Phase 6 — Dashboard

### M6.1 App shell and data layer
**Build**
- Routes: `/` (Overview: machine list, latest run per machine, stat tiles),
  `/metrics/:metric` (detail), `/compare`, `/trials`.
- Machine selector (multi), synthetic-data badge, unit-aware formatters (`GB/s`, `ns`, …).
- Generated API types; TanStack Query with 60 s stale time; loading/error/empty states.

**Verify**
- Playwright smoke: load `/`, pick a machine, open `cpu_int_ops` detail, see a chart.
- Vitest: formatters, grouping, LTTB.

### M6.2 The 12 metric views and comparison
**Build**
- Chart families (one component each, all Recharts, all `isAnimationActive={false}`):
  - **Throughput vs thread count** (line, one series per machine; derived scaling
    efficiency as a secondary toggle): metrics 1–3.
  - **Bandwidth vs working set** (line, log-x, one series per machine): metrics 4–6.
  - **Latency vs working set** (line, log-x, log-y, with `ReferenceLine`s at each
    machine's L1/L2/L3 sizes): metric 7.
  - **Disk bars** (grouped bar by machine, thread-count facet; p99 as separate lower-is-better chart): metrics 8–12.
  - **Trial distribution** (scatter of value vs trial index + histogram + CoV/mean/p95
    stat tiles): any metric/config.
- Compare page: pick 2–4 machines, one metric; shows the family chart plus a ratio table.
- Load the `dataviz` skill before writing chart code (palette, axes, tooltip rules).

**Verify**
- A checklist in `docs/results/dashboard.md`: each of the 12 metrics with a screenshot
  from seeded data. **Target #5 (part 1) check.**
- Latency chart shows step edges at the synthetic machines' configured cache sizes.

**Pitfalls**
- Recharts `XAxis type="number" scale="log"` needs explicit `domain` and ticks.
- Tooltip on multi-series line charts re-renders everything; memoize series data.

### M6.3 10K+ points, smoothly
**Build**
- Trials page fetches `/v1/trials?limit=10000` for one config (or multiple configs
  summing to ≥ 10K) and renders a `ScatterChart`.
- Strategy ladder, measured in order and stopping when the budget is met:
  1. Raw Recharts scatter, no animation, `shape` = minimal circle, no per-point keys.
  2. Client-side LTTB (`src/lib/lttb.ts`) down to 2,000 points for the **line** views;
     scatter keeps all points but uses a lightweight custom `shape`.
  3. If still over budget: canvas layer for the scatter points via a Recharts
     `<Customized>` component drawing to a `<canvas>` overlay, with SVG kept for axes,
     tooltip and brush (a hover index is computed from a sorted x-array, not from 10K
     SVG hit targets).
- Definition of "smoothly": initial React commit for the chart ≤ 500 ms (React Profiler),
  no Long Task > 50 ms during 10 s of mouse hover and a brush-zoom (Chrome Performance
  panel), Lighthouse TBT for the page ≤ 300 ms.

**Verify**
- **Target #5 (part 2) check:** `docs/results/dashboard.md` records which rung of the
  ladder was needed, the Profiler commit time, and the max Long Task, with a screenshot
  of the Performance panel. Also record the numbers at 1K / 10K / 50K points.
- Vitest for LTTB: output length, first/last point preserved, peaks preserved on a
  synthetic spike dataset.

**Pitfalls**
- 10K SVG `<circle>` elements is ~10K DOM nodes; hover recomputation in Recharts is
  O(n) per mouse move. Expect rung 3 to be needed for true smoothness; that is a fine
  outcome to document.
- Never downsample the scatter for the Trials page silently; the point of the page is
  to show every trial. Downsample lines, canvas-render scatters.

### M6.4 Dashboard tests and build
**Build**
- CI job: `npm ci && npm run typecheck && npm run lint && npm run test && npm run build`.
- Bundle size budget (`vite build` report) noted; Recharts is heavy, lazy-load chart
  pages.

**Verify**
- CI green; `dist/` served by nginx in compose shows the same pages as dev.

---

## Phase 7 — Deploy and CI

### M7.1 Docker and compose, final
**Build**
- `deploy/docker/engine-builder.Dockerfile`: builds the engine with `-march=x86-64-v3`
  for people who want a portable binary; documented as *not* the way to get real numbers.
- Compose profiles: `default` (pg, redis, api, dashboard), `loadtest` (adds tuned pg
  settings + k6 service), `observability` (prom + grafana).
- Healthchecks on every service; `depends_on: condition: service_healthy`.
- Root `Makefile` complete: `up`, `down`, `logs`, `test` (all three), `bench`
  (engine `--all --post`), `seed`, `load`, `results` (regenerates `docs/results/README.md`
  tables from committed JSON with a small Go or Python script).

**Verify**
- Fresh clone → `make up && make seed && open http://localhost:3000` works on a machine
  with only Docker (document in README; test by cloning into a temp dir).

### M7.2 CI
**Build**
- GitHub Actions: engine (ubuntu-24.04, gcc-13, build + ctest, asan preset), api
  (go test + integration with service containers pg/redis), dashboard (M6.4), and a
  `compose-smoke` job that brings the stack up and curls `/readyz` + POSTs a fixture run.
  A nightly `k6-smoke` at 200 rps (CI runners cannot validate the 1000 rps target; say so).

**Verify**
- All jobs green on `main`; badge in README.

**Pitfalls**
- CI runners lack `clflushopt` guarantees and have noisy neighbours; engine tests that
  assert timing ratios (flush test, false-sharing test) use loose bounds and are marked
  `[perf]` so they can be skipped with `-E perf` in CI if flaky.

### M7.3 AWS (optional stretch)
**Build**
- `deploy/aws/` Terraform: VPC, ECR (api, dashboard), ECS Fargate services, RDS Postgres
  16 (smallest), ElastiCache Redis (smallest), ALB with `/v1/*` → api and `/*` → dashboard.
  `make aws-plan`, `make aws-apply`, `make aws-destroy`. Cost note in README.

**Verify**
- `terraform plan` clean; one apply/destroy cycle with a screenshot of the dashboard at
  the ALB URL and the ingest of one real run from the laptop.

**Pitfalls**
- Do not leave it running; destroy is part of the verification.

---

## Phase 8 — Results and write-up

### M8.1 Methodology doc and results
**Build**
- `docs/methodology.md`: for each of the 12 metrics, the op definition, the kernel, the
  cold-cache treatment, the timed region, and known limitations (WSL2, VHDX, hybrid cores).
- `docs/results/README.md`: generated tables for all five targets with the real measured
  numbers, the date, and the run conditions. Where a target is missed, the section says
  so and links the analysis.
- `README.md`: architecture picture, quick start, the results table, and a
  "what I learned" section (variance sources, write-allocate, prefetchers vs Sattolo,
  Recharts limits).

**Verify**
- Every number in README traces to a file under `docs/results/` (grep test in CI that
  README numbers appear in a results file is overkill; a manual checklist is fine).

---

## Target metric ↔ milestone map

| Target | Definition (short) | Measured in | Recorded at |
|---|---|---|---|
| 10M+ ops/s | Σ threads' ops ÷ trial wall time (barrier release → last finish), best thread count, ops as defined in CLAUDE.md | M2.4 | `docs/results/cpu_<date>.json` |
| CoV ≤ 3 % over ≥ 1000 trials | sample stddev ÷ mean, raw, post-warmup, cold mode | M2.5, M3.3 | `docs/results/variance_<date>.md` |
| 100K+ measurements ingested/queried | `count(*) ≥ 100000` via real ingest path; all endpoints correct over it | M5.2 | `docs/results/seed_<date>.md` |
| 1000+ rps, p95 ≤ 50 ms | k6 constant-arrival-rate 1000/s × 60 s, `dropped_iterations = 0`, `p(95) < 50` | M5.4 | `docs/results/k6_<date>.json` |
| 12 metrics, 10K+ points smoothly | all 12 charted; ≥ 10K points, commit ≤ 500 ms, no Long Task > 50 ms | M6.2, M6.3 | `docs/results/dashboard.md` |

## Suggested order and rough effort

1. M0.1 → M0.2 → M1.1 → M1.2 → M1.3 → M1.4 → M1.5 → M1.6  (thin slice, ~1 week of evenings)
2. M2.1 → M2.2 → M2.3 → M2.4 → M2.5  (engine core; M2.5 is a long-running study)
3. M5.1 → M5.2 → M5.3 → M5.4  (API scale; can be done in parallel with Phase 3/4 since
   the schema is fixed)
4. M3.1 → M3.2 → M3.3 → M4.1  (memory, disk)
5. M6.1 → M6.2 → M6.3 → M6.4
6. M7.1 → M7.2 → M8.1 → (M7.3)
