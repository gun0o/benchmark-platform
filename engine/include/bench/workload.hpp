// Workload interface (a concept) and the static registry used by `bench list`.
#pragma once

#include "bench/metric.hpp"

#include <concepts>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bench {

struct WorkloadContext {
    int thread_index = 0;
    std::uint64_t working_set_bytes = 0;
    std::uint64_t seed = 0;
};

// A workload is a value type created once per worker thread. setup() may allocate;
// run_batch() must not. run_batch() returns the number of operations it performed,
// where "operation" is defined per metric in CLAUDE.md.
template <class W>
concept WorkloadImpl = requires(W w, const WorkloadContext& ctx) {
    { w.setup(ctx) } -> std::same_as<void>;
    { w.run_batch() } -> std::same_as<std::uint64_t>;
    { w.teardown() } -> std::same_as<void>;
};

struct WorkloadDesc {
    Workload kind;
    std::vector<Metric> metrics;
    std::string_view description;
    bool implemented; // false until its milestone lands
};

const std::vector<WorkloadDesc>& workload_registry();

} // namespace bench
