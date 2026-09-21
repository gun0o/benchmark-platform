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
    // The cheapest delta seen. Between preemptions a now() call costs what it costs, so
    // this is the build's true clock cost even on a busy host - which is what makes
    // mean/min a contention signal that does not care whether the build is instrumented.
    std::uint64_t min_call_ns = 0;
    std::uint64_t samples = 0;    // how many deltas were actually taken
    std::uint64_t elapsed_ns = 0; // how long the sampling window really was
    bool monotonic = true;
    bool ok = false; // monotonic && mean_call_ns <= kMaxMeanCallNs
    static constexpr double kMaxMeanCallNs = 100.0;

    // Mean cost divided by the cheapest call seen: ~1 whenever every call cost about the
    // same, however expensive that is, and large when a few calls took vastly longer than
    // the rest - which is what being descheduled mid-measurement looks like.
    [[nodiscard]] double contention_ratio() const noexcept {
        return min_call_ns == 0 ? 1.0 : mean_call_ns / static_cast<double>(min_call_ns);
    }
};

// Sample back-to-back now() deltas, stopping at `samples` or `max_window`, whichever comes
// first. Used two ways, and the second is why the time cap exists:
//
//   as a self-test  - is this clocksource sane? A sample count is the natural budget.
//   as a canary     - was the host contended? What matters is how long we watched for,
//                     because contention this misses is contention it cannot report.
//
// A pure sample count makes the window depend on how expensive a call happens to be, which
// varies by an order of magnitude between a release build and a sanitizer build, and grows
// further under the very contention the canary is looking for. A time cap pins the window
// instead, and keeps the canary from eating a caller's --max-seconds budget.
ClockCheck clock_selftest(int samples = 100'000,
                          std::chrono::microseconds max_window = std::chrono::microseconds{
                              0}) noexcept;

// Busy-spin (not sleep) for `ms` so the core ramps to its running frequency.
void busy_spin(std::chrono::milliseconds ms) noexcept;

} // namespace bench
