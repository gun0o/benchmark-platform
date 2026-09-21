// Workload interface (a concept) and the static registry used by `bench list`.
#pragma once

#include "bench/metric.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
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
//
// kBatchOps is that same number as a compile-time constant: run_batch() always does a
// whole batch, so the count is known before the batch runs and is never estimated from the
// clock. The runner reports it as params.batch_ops and a test asserts the two agree.
// It cannot be derived as iters x lanes in general: for cpu_hash one op is one 64-byte
// block, which the four mixing lanes process together rather than one op each.
//
// cold_region() names the bytes whose cache state the workload wants controlled: the
// buffer for a memory workload, the input block for cpu_hash, the workload's own lane
// state for cpu_int/cpu_fp. The runner flushes or evicts exactly this before every trial
// when --cold is on, so that trial 1 and trial 500 start from the same cache state. A
// workload with genuinely nothing to cool returns an empty span and the runner records
// params.cold = "n/a" rather than claiming a cold start it did not perform.
template <class W>
concept WorkloadImpl = requires(W w, const W cw, const WorkloadContext& ctx) {
    { std::bool_constant<(W::kBatchOps > 0)>{} } -> std::same_as<std::true_type>;
    { w.setup(ctx) } -> std::same_as<void>;
    { w.run_batch() } -> std::same_as<std::uint64_t>;
    { cw.cold_region() } -> std::same_as<std::span<const std::byte>>;
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
