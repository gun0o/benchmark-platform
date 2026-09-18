#include "bench/timing.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>

using namespace bench;

TEST(Timing, ClockSelfTestPasses) {
    const ClockCheck c = clock_selftest();
    EXPECT_TRUE(c.monotonic);
    EXPECT_GT(c.resolution_ns, 0u);
    EXPECT_LE(c.resolution_ns, 1000u) << "coarse clocksource";
    EXPECT_LE(c.mean_call_ns, ClockCheck::kMaxMeanCallNs)
        << "now() is expensive: vDSO/TSC not in use?";
    EXPECT_TRUE(c.ok);
}

TEST(Timing, SteadyClockIsCLOCK_MONOTONIC) {
    // libstdc++ implements steady_clock with CLOCK_MONOTONIC; sanity-check they agree.
    timespec ts{};
    ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
    const auto raw = std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec};
    const auto now = Clock::now().time_since_epoch();
    EXPECT_LT(std::chrono::abs(now - raw), std::chrono::milliseconds{50});
}

TEST(Timing, TimerMeasuresSleep) {
    Timer t;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto ns = t.elapsed_ns();
    EXPECT_GE(ns, 20'000'000u);
    EXPECT_LT(ns, 500'000'000u) << "scheduler took far too long to wake us";
    t.reset();
    EXPECT_LT(t.elapsed_ns(), 1'000'000u);
}

TEST(Timing, BusySpinTakesAtLeastRequested) {
    Timer t;
    busy_spin(std::chrono::milliseconds{10});
    EXPECT_GE(t.elapsed_ns(), 10'000'000u);
    busy_spin(std::chrono::milliseconds{0}); // must return immediately
}

struct alignas(64) SixtyFourBytes {
    std::uint64_t words[8];
};
static_assert(sizeof(SixtyFourBytes) == 64);

TEST(Timing, DoNotOptimizeCompilesForCommonTypes) {
    int i = 1;
    double d = 2.0;
    std::uint64_t u = 3;
    std::uint64_t* p = &u;
    SixtyFourBytes s{};
    const int ci = 4;
    DoNotOptimize(i);
    DoNotOptimize(d);
    DoNotOptimize(u);
    DoNotOptimize(p);
    DoNotOptimize(s);
    DoNotOptimize(ci);
    ClobberMemory();
    EXPECT_EQ(i, 1);
    EXPECT_EQ(d, 2.0);
    EXPECT_EQ(*p, 3u);
}

TEST(Timing, DoNotOptimizeKeepsLoopAlive) {
    // A loop whose result is only observed through DoNotOptimize must still take time
    // proportional to its trip count. 20M vs 2M iterations of a dependent multiply.
    auto work = [](std::uint64_t n) {
        std::uint64_t x = n | 1;
        for (std::uint64_t k = 0; k < n; ++k)
            x = x * 6364136223846793005ull + k;
        DoNotOptimize(x);
    };
    Timer t;
    work(2'000'000);
    const auto small = t.elapsed_ns();
    t.reset();
    work(20'000'000);
    const auto big = t.elapsed_ns();
    EXPECT_GT(big, small * 5) << "loop was optimized away or folded";
}
