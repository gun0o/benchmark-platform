// mem_latency: nanoseconds per dependent load in a random cyclic pointer chase.
//
// This is the companion to mem_bw and the opposite measurement. A bandwidth kernel walks
// memory in order, so the hardware prefetcher can run ahead and the loads overlap; it
// answers "how much can the machine move per second". A chase makes each load's *address*
// depend on the previous load's *result*, so nothing can overlap and nothing can be
// predicted, and it answers "how long does one access take". The two numbers disagree by
// more than an order of magnitude on the same working set, and both are correct.
#pragma once

#include "bench/cache.hpp"
#include "bench/result.hpp" // json, for describe()
#include "bench/workload.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {

class MemLatencyWorkload {
public:
    // 64 MiB: past this machine's 24 MiB L3, so a bare run reports DRAM latency. The point
    // of the metric is the sweep, but the default should be the number people mean by
    // "memory latency" rather than a cache's.
    static constexpr std::uint64_t kDefaultWorkingSet = 64ull << 20;

    // 64 Ki dependent loads per batch, not PLAN.md's 1 Mi. A DRAM-resident load costs about
    // 100 ns here, so a 1 Mi batch would take 100 ms - twice the default 50 ms trial, which
    // the trial loop can only overshoot, never cut short. 64 Ki keeps the worst-case batch
    // at ~6.5 ms (13 % of a trial) while an L1-resident batch is still ~65 us, four orders
    // of magnitude above the cost of the one clock read that brackets it.
    static constexpr std::uint64_t kBatchLoads = 1u << 16;

    // A chase needs at least two lines to be a cycle at all.
    static constexpr std::uint64_t kMinLines = 2;

    [[nodiscard]] static std::uint64_t buffer_bytes_for(std::uint64_t working_set) noexcept;

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

    [[nodiscard]] std::uint64_t batch_units() const noexcept { return kBatchLoads; }
    [[nodiscard]] std::span<const std::byte> cold_region() const { return buf_.span(); }
    [[nodiscard]] json describe() const;

    // For tests.
    [[nodiscard]] std::uint64_t lines() const noexcept { return lines_; }
    [[nodiscard]] std::uint64_t buffer_bytes() const noexcept { return buf_.size(); }
    [[nodiscard]] bool huge_requested() const noexcept { return buf_.huge_requested(); }
    [[nodiscard]] std::uint64_t huge_granted_bytes() const noexcept {
        return buf_.huge_granted_bytes();
    }
    // The chase as line indices: next_line(i) is the line the chase goes to from line i.
    [[nodiscard]] std::uint64_t next_line(std::uint64_t line) const noexcept;

private:
    Buffer buf_;
    std::uint64_t lines_ = 0;
    std::uint64_t idx_ = 0; // current position, as a uint64_t index into the buffer
};
static_assert(WorkloadImpl<MemLatencyWorkload>);

// Sattolo's algorithm: shuffle `a` (initially any permutation of 0..n-1) into a permutation
// whose functional form i -> a[i] is a *single* n-cycle.
//
// It is Fisher-Yates with one character changed - the random index is drawn from [0, i)
// rather than [0, i] - and that one character is the whole point. Fisher-Yates produces a
// uniformly random permutation, which will usually decompose into several disjoint cycles;
// a chase that fell into a short one would visit a handful of lines forever and report the
// latency of whatever cache those few lines fit in, regardless of the working set. Sattolo
// produces a uniformly random *cyclic* permutation, so the chase visits every line exactly
// once per lap, by construction rather than by luck.
//
// Exposed and tested directly: this is the one place where a subtle mistake would produce
// a plausible-looking number that is wrong by a factor of 30.
void sattolo_shuffle(std::span<std::uint64_t> a, std::uint64_t seed) noexcept;

} // namespace bench
