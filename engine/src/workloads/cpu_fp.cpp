#include "bench/workloads/cpu_fp.hpp"

#include "bench/timing.hpp"

#include <cmath>
#include <random>

namespace bench {

void CpuFpWorkload::setup(const WorkloadContext& ctx) {
    // Runtime-seeded, like cpu_int: a compile-time-known b or c would let GCC fold the
    // recurrence, and a compile-time-known lane start would let it precompute the answer.
    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15;
    std::mt19937_64 rng{ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 2))};
    std::uniform_real_distribution<double> b_dist{0.5, 0.75};
    std::uniform_real_distribution<double> c_dist{0.25, 0.5};
    std::uniform_real_distribution<double> a_dist{1.0, 2.0};
    for (int l = 0; l < kLanes; ++l) {
        const auto i = static_cast<std::size_t>(l);
        a_[i] = a_dist(rng);
        b_[i] = b_dist(rng);
        c_[i] = c_dist(rng);
    }
}

// Why the value range is a correctness question, not an aesthetic one:
//
//   * If |b| > 1 the lanes run away to +inf in a few thousand iterations, and from then on
//     every FMA is inf*b + c = inf. The instruction still issues, but the benchmark would
//     be measuring arithmetic on infinities, which is not what "a double-precision FMA"
//     is supposed to mean.
//   * If the lanes decayed to zero they would pass through the subnormal range on the way,
//     and subnormal operands cost tens of extra cycles on most x86 parts. Throughput would
//     collapse partway through a trial and the number would depend on how long the trial
//     ran - the opposite of what a rate is supposed to be.
//
// b in [0.5, 0.75) with c in [0.25, 0.5) avoids both: the map is a contraction towards
// c / (1 - b), which lands in [0.5, 2). Every operand stays a normal double for the whole
// run, so every FMA costs the same, and the same 8 lanes at trial 1 and trial 1000 are in
// the same numerical regime. The lanes converge to a constant within ~50 iterations, which
// is fine: FMA timing on normal doubles does not depend on the operand values.
std::uint64_t CpuFpWorkload::run_batch() {
    std::array<double, kLanes> a = a_;
    const std::array<double, kLanes> b = b_, c = c_;
    for (std::uint64_t i = 0; i < kItersPerBatch; ++i) {
        for (int l = 0; l < kLanes; ++l) {
            const auto j = static_cast<std::size_t>(l);
            a[j] = std::fma(a[j], b[j], c[j]); // one op
        }
    }
    a_ = a;
    double fold = 0.0;
    for (double v : a)
        fold += v;
    sink_ += fold;
    DoNotOptimize(sink_);
    return kBatchOps;
}

} // namespace bench
