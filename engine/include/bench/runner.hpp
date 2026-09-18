// Trial loop. M1.1: single thread, one batch per trial, naive timing.
// M1.2 adds warmup + time-boxed trials; M2.1 adds the worker pool and barrier.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bench/metric.hpp"
#include "bench/result.hpp"

namespace bench {

struct RunConfig {
    std::vector<Workload> workloads;
    std::vector<int> thread_counts{1};
    std::vector<std::uint64_t> working_sets{0};
    int trials = 1;
    int trial_ms = 50; // accepted now, used from M1.2
    std::uint64_t seed = 0;
    bool verbose = false;
};

// Progress callback: called once per completed result (for --verbose).
using ProgressFn = std::function<void(const Result&)>;

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine,
                           std::vector<std::string> argv, const ProgressFn& progress = {});

} // namespace bench
