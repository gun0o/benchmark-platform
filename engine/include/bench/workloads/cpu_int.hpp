// cpu_int: one op = one update of one 64-bit lane: acc = (acc * K) + (acc >> 17) ^ i.
#pragma once

#include "bench/workload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {

class CpuIntWorkload {
public:
    static constexpr int kLanes = 8;
    static constexpr std::uint64_t kItersPerBatch =
        1u << 17; // 131072 iters * 8 lanes = 1 Mi ops per batch

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

    // cpu_int has no input buffer: its whole working set is the 8 lanes, the multiplier and
    // the sink, which live in this object. Flushing it makes every trial reload that state
    // from memory, so no trial inherits a warm copy from the one before. It is 128 bytes,
    // so the effect on a 50 ms trial is unmeasurable by construction - which is the point:
    // the mechanism is uniform across workloads and its cost here is known to be nil.
    [[nodiscard]] std::span<const std::byte> cold_region() const {
        return std::as_bytes(std::span<const CpuIntWorkload>{this, 1});
    }

private:
    std::array<std::uint64_t, kLanes> lanes_{};
    std::uint64_t k_ = 0;
    std::uint64_t sink_ = 0;
};
static_assert(WorkloadImpl<CpuIntWorkload>);

} // namespace bench
