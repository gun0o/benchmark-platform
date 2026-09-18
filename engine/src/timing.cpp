#include "bench/timing.hpp"

#include <algorithm>
#include <ctime>
#include <vector>

namespace bench {

std::uint64_t clock_resolution_ns() noexcept {
    timespec ts{};
    if (clock_getres(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

ClockCheck clock_selftest(int samples) noexcept {
    ClockCheck c;
    c.resolution_ns = clock_resolution_ns();
    if (samples < 2)
        samples = 2;
    // Warm the vDSO path once so the first call's page-in does not skew the mean.
    auto prev = Clock::now();
    prev = Clock::now();
    std::uint64_t total = 0;
    for (int i = 0; i < samples; ++i) {
        const auto now = Clock::now();
        if (now < prev)
            c.monotonic = false;
        const std::uint64_t d = ns_between(prev, now);
        total += d;
        c.max_call_ns = std::max(c.max_call_ns, d);
        prev = now;
    }
    c.mean_call_ns = static_cast<double>(total) / static_cast<double>(samples);
    c.ok = c.monotonic && c.mean_call_ns <= ClockCheck::kMaxMeanCallNs;
    return c;
}

void busy_spin(std::chrono::milliseconds ms) noexcept {
    if (ms.count() <= 0)
        return;
    const auto end = Clock::now() + ms;
    std::uint64_t x = 1;
    while (Clock::now() < end) {
        for (int i = 0; i < 1000; ++i)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        DoNotOptimize(x);
    }
}

} // namespace bench
