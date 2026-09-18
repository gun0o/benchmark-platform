// Timing primitives. Only steady_clock is used anywhere in the engine.
#pragma once

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace bench {

using Clock = std::chrono::steady_clock;

// Force the compiler to assume `v` is read and may have been modified, so work that
// produced it cannot be dead-code-eliminated. Google Benchmark style.
// Register-sized trivially-copyable values may live in a register ("r") or memory ("m");
// anything larger must be in memory, otherwise GCC reports an impossible constraint.
template <class T> inline void DoNotOptimize(T& v) noexcept {
    if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(void*)) {
        asm volatile("" : "+m,r"(v) : : "memory");
    } else {
        asm volatile("" : "+m"(v) : : "memory");
    }
}
template <class T> inline void DoNotOptimize(const T& v) noexcept {
    if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(void*)) {
        asm volatile("" : : "m,r"(v) : "memory");
    } else {
        asm volatile("" : : "m"(v) : "memory");
    }
}

// Compiler barrier: all pending memory writes must be materialized before continuing.
inline void ClobberMemory() noexcept {
    asm volatile("" : : : "memory");
}

inline std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Small stopwatch over steady_clock.
class Timer {
public:
    Timer() noexcept : start_(Clock::now()) {}
    void reset() noexcept { start_ = Clock::now(); }
    [[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
        return ns_between(start_, Clock::now());
    }
    [[nodiscard]] double elapsed_s() const noexcept {
        return static_cast<double>(elapsed_ns()) / 1e9;
    }
    [[nodiscard]] Clock::time_point start() const noexcept { return start_; }

private:
    Clock::time_point start_;
};

// clock_getres(CLOCK_MONOTONIC) in nanoseconds (steady_clock is CLOCK_MONOTONIC on libstdc++).
std::uint64_t clock_resolution_ns() noexcept;

// Self-test of the clocksource: back-to-back now() calls should be cheap (mean <= 100 ns
// on a sane vDSO/TSC setup) and never go backwards.
struct ClockCheck {
    std::uint64_t resolution_ns = 0;
    double mean_call_ns = 0; // mean delta between consecutive now() calls
    std::uint64_t max_call_ns = 0;
    bool monotonic = true;
    bool ok = false; // monotonic && mean_call_ns <= kMaxMeanCallNs
    static constexpr double kMaxMeanCallNs = 100.0;
};
ClockCheck clock_selftest(int samples = 100'000) noexcept;

// Busy-spin (not sleep) for `ms` so the core ramps to its running frequency.
void busy_spin(std::chrono::milliseconds ms) noexcept;

} // namespace bench
