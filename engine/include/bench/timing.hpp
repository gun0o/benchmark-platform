// Timing primitives. Only steady_clock is used anywhere in the engine.
// M1.2 extends this with Timer, clock_resolution_ns() and the clock self-test.
#pragma once

#include <chrono>
#include <cstdint>

namespace bench {

using Clock = std::chrono::steady_clock;

// Force the compiler to assume `v` is read and may have been modified.
template <class T> inline void DoNotOptimize(T& v) noexcept {
    asm volatile("" : "+r,m"(v) : : "memory");
}
template <class T> inline void DoNotOptimize(const T& v) noexcept {
    asm volatile("" : : "r,m"(v) : "memory");
}

// Compiler barrier: all pending memory writes must be materialized.
inline void ClobberMemory() noexcept { asm volatile("" : : : "memory"); }

inline std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

} // namespace bench
