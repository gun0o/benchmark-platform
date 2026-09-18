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

#include "bench/metric.hpp"
#include "bench/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bench {

struct RunConfig {
    std::vector<Workload> workloads;
    std::vector<int> thread_counts{1};
    std::vector<std::uint64_t> working_sets{0};
    int trials = 1;        // timed trials emitted per configuration
    int warmup_trials = 5; // at least this many warmup trials per configuration, never emitted
    int warmup_ms = 500;   // ...and warmup continues until at least this much time has passed
    int trial_ms = 50;     // each worker runs whole batches until this much time has elapsed
    int spin_ms = 500;     // one global busy spin before the first configuration (frequency ramp)
    std::uint64_t seed = 0;
    bool pin = false;      // pin worker i to cpus[i % cpus.size()]
    std::vector<int> cpus; // empty => default_cpu_list(logical_cpus)
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
