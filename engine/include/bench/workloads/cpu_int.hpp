// cpu_int: one op = one update of one 64-bit lane: acc = (acc * K) + (acc >> 17) ^ i.
#pragma once

#include <array>
#include <cstdint>

#include "bench/workload.hpp"

namespace bench {

class CpuIntWorkload {
public:
    static constexpr int kLanes = 8;
    static constexpr std::uint64_t kItersPerBatch = 1u << 20; // ops per batch = iters * lanes

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

private:
    std::array<std::uint64_t, kLanes> lanes_{};
    std::uint64_t k_ = 0;
    std::uint64_t sink_ = 0;
};
static_assert(WorkloadImpl<CpuIntWorkload>);

} // namespace bench
