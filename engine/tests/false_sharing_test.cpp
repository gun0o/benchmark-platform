// A/B: eight threads each bumping their own counter with an atomic read-modify-write,
// once with counters in separate 128-byte blocks and once packed 8 bytes apart on one
// cache line. The packed layout makes the cores fight over the line (false sharing).
//
// Why fetch_add and not a plain store: a relaxed store loop is absorbed by the store
// buffer and a load+store loop is served by store-to-load forwarding, so neither needs
// the cache line at every iteration and neither shows the effect (measured 1.01x and
// 1.03x on a Core Ultra 9 185H). A locked RMW must own the line at every iteration and
// showed 11.5x. Per-thread counters in real code are usually RMWs, so that is the honest
// pattern to test.
#include "bench/timing.hpp"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 8;
constexpr std::uint64_t kIters = 5'000'000;

struct alignas(128) Padded {
    std::atomic<std::uint64_t> counter{0};
};
struct Packed {
    std::atomic<std::uint64_t> counter{0};
};
static_assert(sizeof(Padded) == 128);
static_assert(sizeof(Packed) == 8);

template <class SlotT> std::uint64_t run_ns() {
    std::vector<SlotT> slots(kThreads);
    std::vector<std::jthread> threads;
    bench::Timer t;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&slots, i] {
            auto& c = slots[static_cast<std::size_t>(i)].counter;
            for (std::uint64_t k = 0; k < kIters; ++k)
                c.fetch_add(1, std::memory_order_relaxed);
        });
    threads.clear(); // join
    for (auto& s : slots)
        EXPECT_EQ(s.counter.load(), kIters);
    return t.elapsed_ns();
}

} // namespace

TEST(FalseSharing, PaddedSlotsAreFasterThanPacked) {
    if (std::thread::hardware_concurrency() < kThreads)
        GTEST_SKIP() << "fewer than 8 CPUs";
    run_ns<Padded>(); // warm up threads / frequency
    std::uint64_t best_padded = UINT64_MAX, best_packed = UINT64_MAX;
    for (int rep = 0; rep < 3; ++rep) {
        best_padded = std::min(best_padded, run_ns<Padded>());
        best_packed = std::min(best_packed, run_ns<Packed>());
    }
    const double ratio = static_cast<double>(best_packed) / static_cast<double>(best_padded);
    RecordProperty("padded_ms", static_cast<double>(best_padded) / 1e6);
    RecordProperty("packed_ms", static_cast<double>(best_packed) / 1e6);
    RecordProperty("packed_over_padded", ratio);
    std::printf("[ false-sharing ] padded %.1f ms, packed %.1f ms, packed/padded = %.2fx\n",
                static_cast<double>(best_padded) / 1e6, static_cast<double>(best_packed) / 1e6,
                ratio);
    EXPECT_GE(ratio, 1.3) << "packed counters should be at least 1.3x slower than padded";
}
