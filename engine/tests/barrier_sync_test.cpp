// Three-way A/B for the synchronized start of a trial, 8 threads, spread over 100 trials
// between the earliest and latest per-thread start stamp (median and max reported; the
// assertions use medians because single-trial maxima on a VM are host scheduling noise):
//   (a) no barrier: each thread stamps as soon as it is spawned;
//   (b) std::barrier (arrive_and_wait) at both ends of the trial: waiters sleep on a futex
//       and are woken one by one by the kernel (on a VM, after un-parking the vCPU);
//   (c) bench::SpinBarrier at both ends, which is what the runner uses.
#include "bench/sync.hpp"
#include "bench/timing.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 8;
constexpr int kTrials = 100;

struct alignas(128) Stamp {
    bench::Clock::time_point start{};
};

std::uint64_t spread_ns(const std::vector<Stamp>& s) {
    auto lo = s[0].start, hi = s[0].start;
    for (const auto& x : s) {
        lo = std::min(lo, x.start);
        hi = std::max(hi, x.start);
    }
    return bench::ns_between(lo, hi);
}

struct Spread {
    double median_us = 0;
    double max_us = 0;
};

Spread summarize(std::vector<std::uint64_t> v) {
    std::sort(v.begin(), v.end());
    return {static_cast<double>(v[v.size() / 2]) / 1e3, static_cast<double>(v.back()) / 1e3};
}

// One pool of kThreads workers plus the calling thread as coordinator. Like the runner,
// the pool warms up for kWarmupMs before the kTrials measured phases: the first ~250 ms
// after spawning a pool show multi-ms spreads on a VM while the hypervisor settles the
// now-busy vCPUs (M2.1 notes). Workers loop until told to stop.
constexpr int kWarmupMs = 400;

template <class Barrier> Spread pooled(Barrier& start, Barrier& done) {
    std::vector<Stamp> stamps(kThreads);
    std::atomic<bool> stop{false};
    std::vector<std::jthread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&, i] {
            for (;;) {
                start.arrive_and_wait();
                if (stop.load(std::memory_order_relaxed))
                    break;
                stamps[static_cast<std::size_t>(i)].start = bench::Clock::now();
                bench::busy_spin(std::chrono::milliseconds{1});
                done.arrive_and_wait();
            }
        });
    const auto t0 = bench::Clock::now();
    while (bench::Clock::now() - t0 < std::chrono::milliseconds{kWarmupMs}) {
        start.arrive_and_wait();
        done.arrive_and_wait();
    }
    std::vector<std::uint64_t> spreads;
    for (int t = 0; t < kTrials; ++t) {
        start.arrive_and_wait();
        done.arrive_and_wait();
        spreads.push_back(spread_ns(stamps));
    }
    stop.store(true, std::memory_order_relaxed);
    start.arrive_and_wait();
    return summarize(std::move(spreads));
}

Spread with_std_barrier() {
    std::barrier<> start(kThreads + 1), done(kThreads + 1);
    return pooled(start, done);
}

Spread with_spin_barrier() {
    bench::SpinBarrier start(kThreads + 1), done(kThreads + 1);
    return pooled(start, done);
}

Spread without_barrier() {
    std::vector<std::uint64_t> spreads;
    for (int t = 0; t < kTrials; ++t) {
        std::vector<Stamp> stamps(kThreads);
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < kThreads; ++i)
                threads.emplace_back([&, i] {
                    stamps[static_cast<std::size_t>(i)].start = bench::Clock::now();
                    bench::busy_spin(std::chrono::milliseconds{1});
                });
        }
        spreads.push_back(spread_ns(stamps));
    }
    return summarize(std::move(spreads));
}

} // namespace

TEST(BarrierSync, SpinBarrierTightensStartSpread) {
    if (std::thread::hardware_concurrency() < kThreads)
        GTEST_SKIP() << "fewer than 8 CPUs";
    const Spread none = without_barrier();
    const Spread sleeping = with_std_barrier();
    const Spread spinning = with_spin_barrier();
    RecordProperty("median_spread_none_us", none.median_us);
    RecordProperty("median_spread_std_barrier_us", sleeping.median_us);
    RecordProperty("median_spread_spin_barrier_us", spinning.median_us);
    std::printf("[ barrier-sync ] start spread over %d trials, %d threads (median / max us): "
                "none %.1f / %.0f, std::barrier %.1f / %.0f, SpinBarrier %.2f / %.0f\n",
                kTrials, kThreads, none.median_us, none.max_us, sleeping.median_us, sleeping.max_us,
                spinning.median_us, spinning.max_us);
    EXPECT_LT(spinning.median_us, sleeping.median_us)
        << "SpinBarrier should beat std::barrier's futex wake-up";
    EXPECT_LT(spinning.median_us, none.median_us)
        << "SpinBarrier should beat an unsynchronized start";
    EXPECT_LT(spinning.median_us, 50.0) << "typical trial: 8 spinning threads start within 50 us";
}

TEST(SpinBarrier, CompletionRunsOncePerPhaseAndAllPhasesComplete) {
    constexpr int n = 4, phases = 1000;
    std::atomic<int> completions{0};
    bench::SpinBarrier b(n, [&] { completions.fetch_add(1, std::memory_order_relaxed); });
    std::vector<std::jthread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back([&] {
            for (int p = 0; p < phases; ++p)
                b.arrive_and_wait();
        });
    threads.clear();
    EXPECT_EQ(completions.load(), phases);
    EXPECT_EQ(b.generation(), static_cast<std::uint64_t>(phases));
}
