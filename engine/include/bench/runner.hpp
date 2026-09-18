// Trial loop. M1.2: single thread, warmup trials discarded, each timed trial is
// time-boxed to trial_ms and runs whole batches. M2.1 adds the worker pool and barrier.
#pragma once

#include "bench/metric.hpp"
#include "bench/result.hpp"

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
    int warmup_trials = 5; // run first, identical to timed trials, never emitted
    int trial_ms = 50;     // each trial runs whole batches until this much time has elapsed
    int spin_ms = 500;     // one global busy spin before the first warmup (frequency ramp)
    std::uint64_t seed = 0;
    bool verbose = false;
};

// Progress callback: called for every trial, warmup ones flagged (they are not emitted).
using ProgressFn = std::function<void(const Result&, bool warmup)>;

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine,
                           std::vector<std::string> argv, const ProgressFn& progress = {});

} // namespace bench
