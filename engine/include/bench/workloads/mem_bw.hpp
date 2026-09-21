// mem_bw: streaming read / write / copy bandwidth over a per-thread buffer.
//
// One workload class, three metrics. Which one a session measures is fixed at setup() from
// ctx.metric and never changes inside a trial: a trial that alternated reads and writes
// would report a number that is neither.
#pragma once

#include "bench/cache.hpp"
#include "bench/result.hpp" // json, for describe()
#include "bench/workload.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bench {

class MemBwWorkload {
public:
    enum class Mode { read, write, copy };

    // 64 MiB per thread: comfortably past this machine's 24 MiB L3, so the default run
    // reports DRAM bandwidth rather than a cache's. Sweeping --working-set is what exposes
    // the cache levels; the default is the number most people mean by "memory bandwidth".
    static constexpr std::uint64_t kDefaultWorkingSet = 64ull << 20;

    // A batch is a whole number of passes over the buffer, and never a short one: at 4 KiB
    // a single pass takes ~40 ns, which is about three steady_clock::now() calls, so timing
    // one pass at a time would be measuring the clock. Passes are repeated until a batch
    // moves at least this much, which puts the per-batch clock cost under 0.05 % everywhere
    // in the sweep while keeping a 256 MiB batch to exactly one pass.
    static constexpr std::uint64_t kMinBatchBytes = 4ull << 20;

    // Buffers are a whole number of cache lines (and so of 32-byte AVX stores).
    [[nodiscard]] static std::uint64_t buffer_bytes_for(std::uint64_t working_set) noexcept;
    [[nodiscard]] static Mode mode_of(Metric m) noexcept;

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

    [[nodiscard]] std::uint64_t batch_units() const noexcept { return batch_bytes_; }

    // The whole allocation: the buffer for read and write, both halves for copy. Everything
    // the trial will touch, so --cold clflush cools all of it (and the runner records "n/a"
    // once that exceeds L3, which is the honest answer for a DRAM-sized working set).
    [[nodiscard]] std::span<const std::byte> cold_region() const { return buf_.span(); }

    [[nodiscard]] json describe() const;

    // For tests.
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint64_t buffer_bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t passes_per_batch() const noexcept { return passes_; }
    [[nodiscard]] bool non_temporal() const noexcept { return nt_; }
    [[nodiscard]] std::span<const std::byte> source() const { return {buf_.data(), bytes_}; }
    [[nodiscard]] std::span<const std::byte> destination() const {
        return {buf_.data() + (mode_ == Mode::copy ? bytes_ : 0), bytes_};
    }
    [[nodiscard]] std::uint64_t digest() const noexcept { return acc_; }

    // True when this build can issue 32-byte non-temporal stores. --nt on a build without
    // them is recorded as requested-but-not-effective rather than silently ignored.
    [[nodiscard]] static bool non_temporal_available() noexcept;

private:
    Buffer buf_;
    Mode mode_ = Mode::read;
    std::uint64_t bytes_ = 0;       // per-direction bytes, i.e. the working set
    std::uint64_t passes_ = 1;      // full passes per batch
    std::uint64_t batch_bytes_ = 0; // passes_ * bytes_, the unit count run_batch() returns
    bool nt_ = false;               // --nt asked for AND available
    bool nt_requested_ = false;
    std::uint64_t fill_ = 0; // runtime store value: the compiler cannot fold the write loop
    std::uint64_t acc_ = 0;  // read checksum, sunk through DoNotOptimize
};
static_assert(WorkloadImpl<MemBwWorkload>);

} // namespace bench
