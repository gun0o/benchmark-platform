// Trial loop with a per-configuration worker pool.
//
// For thread_count = N the runner spawns N std::jthreads. Every trial is bracketed by two
// std::barriers that the main thread also joins: the start barrier's completion function
// stamps t_release, the end barrier's stamps t_done. Aggregate throughput for a trial is
// sum(ops_i) / (t_done - t_release). Workers spin (never sleep) at both barriers so their
// CPUs stay hot between trials; see runner.cpp for the measurements behind that. Each
// worker records its own start/end stamps and ops in a 128-byte-aligned slot so per-thread
// numbers and the start spread can be reported without false sharing between workers.
#pragma once

#include "bench/cache.hpp"
#include "bench/metric.hpp"
#include "bench/result.hpp"
#include "bench/workload.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bench {

struct RunConfig {
    // What to measure. `metrics` is what the runner actually walks: one configuration per
    // metric, because mem_bw's read, write and copy (and the disk workloads' reads and
    // writes) are separate measurements that share a class, not one measurement with three
    // answers. `workloads` is the convenient shorthand - run_benchmarks expands it into
    // every metric the workload owns when `metrics` is left empty.
    std::vector<Workload> workloads;
    std::vector<Metric> metrics;
    std::vector<int> thread_counts{1};
    std::vector<std::uint64_t> working_sets{0};
    int trials = 1;         // timed trials emitted per configuration
    int warmup_trials = 5;  // at least this many warmup trials per configuration, never emitted
    int warmup_ms = 500;    // ...and warmup continues until at least this much time has passed
    int trial_ms = 50;      // each worker runs whole batches until this much time has elapsed
    int spin_ms = 500;      // one global busy spin before the first configuration (frequency ramp)
    double max_seconds = 0; // safety cap on the whole run; 0 = no cap. Stops at a trial boundary.
    std::uint64_t seed = 0;
    ColdMode cold = ColdMode::clflush; // per-trial cache preparation, done before the barrier
    bool pin = false;                  // pin worker i to cpus[i % cpus.size()]
    std::vector<int> cpus;             // empty => default_cpu_list(logical_cpus)
    // Rotate through configurations trial-by-trial instead of finishing one before the
    // next, so a slow drift (thermal, a background process) lands on all of them equally.
    // Off by default: it keeps every configuration's pool alive at once, and those workers
    // park on a futex between their turns rather than spinning.
    bool interleave = false;
    // How many times a configuration may be thrown away and re-run when the clock
    // self-test says the host was contended (M1.2's canary). Sequential runs only.
    int canary_retries = 3;
    WorkloadOptions opts; // knobs owned by the workloads themselves (--nt, --disk-path, ...)
    bool verbose = false;
};

// Stride-2 order (0,2,4,...) then the odd CPUs, so the first N workers avoid landing on
// hyperthread siblings when the topology is the usual "sibling = cpu+1" layout.
std::vector<int> default_cpu_list(int logical_cpus);

// Progress callback: called for every trial, warmup ones flagged (they are not emitted).
using ProgressFn = std::function<void(const Result&, bool warmup)>;

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine,
                           std::vector<std::string> argv, const ProgressFn& progress = {});

// Cooperative abort (Ctrl-C). Async-signal-safe. The current trial finishes, workers stop
// at the next trial boundary, and run_benchmarks returns the results collected so far.
void request_abort() noexcept;
bool abort_requested() noexcept;
void clear_abort() noexcept;

} // namespace bench
