#include "bench/workloads/cpu_int.hpp"

#include "bench/timing.hpp"

#include <random>

namespace bench {

void CpuIntWorkload::setup(const WorkloadContext& ctx) {
    // Runtime-seeded so the compiler cannot constant-fold the lanes or K.
    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15;
    std::mt19937_64 rng{ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 1))};
    for (auto& l : lanes_)
        l = rng();
    k_ = rng() | std::uint64_t{1}; // odd multiplier
}

std::uint64_t CpuIntWorkload::run_batch() {
    std::array<std::uint64_t, kLanes> acc = lanes_;
    const std::uint64_t k = k_;
    for (std::uint64_t i = 0; i < kItersPerBatch; ++i) {
        for (int l = 0; l < kLanes; ++l) {
            std::uint64_t& a = acc[static_cast<std::size_t>(l)];
            a = (a * k) + ((a >> 17) ^ i); // one op: mul, shift, xor, add
        }
    }
    lanes_ = acc;
    std::uint64_t fold = 0;
    for (auto a : acc)
        fold ^= a;
    sink_ ^= fold;
    DoNotOptimize(sink_);
    return kItersPerBatch * static_cast<std::uint64_t>(kLanes);
}

} // namespace bench
