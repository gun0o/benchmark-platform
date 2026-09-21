#include "bench/workloads/cpu_hash.hpp"

#include "bench/timing.hpp"

#include <algorithm>
#include <random>

namespace bench {

std::size_t CpuHashWorkload::buffer_bytes_for(std::uint64_t bytes) noexcept {
    if (bytes == 0)
        return kDefaultBufferBytes;
    const auto blocks = std::max<std::uint64_t>(1, bytes / kHashBlockBytes);
    return static_cast<std::size_t>(blocks) * kHashBlockBytes;
}

void CpuHashWorkload::setup(const WorkloadContext& ctx) {
    const std::size_t bytes = buffer_bytes_for(ctx.working_set_bytes);
    // No huge pages: the whole point of this buffer is that it is small. Asking for a 2 MiB
    // page to back 4 KiB of data would be absurd, and alloc_buffer would ignore it anyway.
    buf_ = alloc_buffer(bytes, AllocOptions{.huge_pages = false, .first_touch = true});
    blocks_ = bytes / kHashBlockBytes;

    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15;
    std::mt19937_64 rng{ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 3))};
    // Random bytes, written at runtime: the compiler cannot know a single byte of the input,
    // so it cannot precompute a single hash.
    for (std::size_t i = 0; i + sizeof(std::uint64_t) <= bytes; i += sizeof(std::uint64_t)) {
        const std::uint64_t v = rng();
        std::memcpy(buf_.data() + i, &v, sizeof(v));
    }
    seed_ = rng();
    next_block_ = 0;
    acc_ = 0;
}

// Two decisions are visible in this loop.
//
// 1. Every block is hashed with the same seed, so blocks are independent of each other.
//    The alternative - feeding each block's hash in as the next block's seed - would make
//    the batch one long dependency chain, and each op would cost the mixer's *latency*
//    rather than its throughput. That is a real number about a real thing, but it is not
//    the number this metric claims: "hashing one 64-byte block" is a unit of work, and
//    ops/s should say how many of them the core can retire per second. Independent blocks
//    let the out-of-order engine overlap them, which is also what any real hashing workload
//    over many separate keys does.
//
// 2. The fold is acc = acc * P1 + h, not acc ^= h. XOR looks like the cheaper choice, and
//    it was the first one written here, but it cancels: the batch is 64 Ki ops over 64
//    blocks, so every block is hashed exactly 1024 times and an even number of XORs of the
//    same value is zero. The digest at the end of every batch was identically 0 - a sink
//    that provably carries no information out of the loop, which is one inlining decision
//    away from letting the compiler delete the loop that fills it.
//
//    Multiply-accumulate cannot cancel and is order-dependent, so the digest is a real
//    checksum over every block the batch touched (tested in cpu_workloads_test.cpp).
//
//    It is not free. Three folds were built as separate binaries and run alternately, 9
//    repetitions of 21 trials each (docs/results/m2.4/hash_fold.log):
//
//      acc ^= h            1.360e8 ops/s   baseline
//      acc += h            1.312e8 ops/s   -3.6 %
//      acc = acc * P1 + h  1.289e8 ops/s   -5.2 %   <- chosen
//
//    So ~3-5 % of the reported rate is the sink rather than the mixer, and cpu_hash_ops is
//    honestly "hash a block and accumulate it", not "hash a block" alone. That is the price
//    of a sink whose value can be checked; XOR's could not be, because it was always 0. The
//    run-to-run spread (+/-6 %) is wider than the gap between the three, so the ordering is
//    consistent but the exact percentage is not precise.
std::uint64_t CpuHashWorkload::run_batch() {
    const std::byte* const base = buf_.data();
    const std::size_t blocks = blocks_;
    const std::uint64_t seed = seed_;
    std::size_t b = next_block_;
    std::uint64_t acc = acc_;
    for (std::uint64_t i = 0; i < kBatchOps; ++i) {
        acc = acc * kHashP1 + hash_block(base + b * kHashBlockBytes, seed); // one op
        b = (b + 1 == blocks) ? 0 : b + 1;
    }
    next_block_ = b;
    acc_ = acc;
    DoNotOptimize(acc_);
    return kBatchOps;
}

} // namespace bench
