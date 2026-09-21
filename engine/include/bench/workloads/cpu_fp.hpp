// cpu_fp: one op = one double-precision fused multiply-add `a = a * b + c` on one lane.
#pragma once

#include "bench/workload.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {

// Eight independent FMA chains. The lane count is the whole design: an FMA on this part has
// ~4 cycles of latency and there are 2 FMA ports, so 4 x 2 = 8 chains are needed before the
// ports are the limit rather than the latency. With fewer lanes the benchmark would report
// FMA *latency* dressed up as throughput (the M1.2 symptom: ops/s ~ core_GHz / latency).
//
// b and c are chosen so the recurrence a <- a*b + c is numerically boring: b in [0.5, 0.75)
// and c in [0.25, 0.5) make it a contraction with the finite fixed point c / (1 - b), which
// lies in [0.5, 2). See cpu_fp.cpp for why that matters for the measurement.
class CpuFpWorkload {
public:
    static constexpr int kLanes = 8;
    static constexpr std::uint64_t kItersPerBatch = 1u << 17;           // 131072 iters * 8 lanes
    static constexpr std::uint64_t kBatchOps = kItersPerBatch * kLanes; // 1 Mi ops per batch

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

    // Fixed at compile time for the CPU kernels; the runner asks for it through this
    // accessor because a memory or disk batch is only sized once setup() has run.
    [[nodiscard]] std::uint64_t batch_units() const noexcept { return kBatchOps; }

    // Same reasoning as cpu_int: the working set is this object's own lane state, so the
    // cold mode flushes it and every trial reloads it from memory. 256 bytes (three
    // 64-byte-aligned lane arrays plus the sink), so the cost is nil by construction.
    [[nodiscard]] std::span<const std::byte> cold_region() const {
        return std::as_bytes(std::span<const CpuFpWorkload>{this, 1});
    }

    // For tests: the lanes converge to the analytic fixed point of a <- a*b + c, which is
    // what proves the kernel computes the operation the metric claims it does.
    [[nodiscard]] std::span<const double> lanes() const { return a_; }
    [[nodiscard]] std::span<const double> mul() const { return b_; }
    [[nodiscard]] std::span<const double> add() const { return c_; }
    [[nodiscard]] static double fixed_point(double b, double c) { return c / (1.0 - b); }

private:
    alignas(64) std::array<double, kLanes> a_{};
    std::array<double, kLanes> b_{};
    std::array<double, kLanes> c_{};
    double sink_ = 0.0;
};
static_assert(WorkloadImpl<CpuFpWorkload>);

} // namespace bench
