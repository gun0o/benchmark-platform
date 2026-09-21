// How wide a window does the clock canary need, and does mean/min separate a quiet host
// from a busy one regardless of build? Prints both the rule M2.5 uses (mean/min ratio)
// and the absolute rule M1.2 specified (mean > 100 ns), so they can be compared.
#include "bench/timing.hpp"
#include <chrono>
#include <cstdio>
using namespace bench;
int main() {
    std::printf("%10s %11s %10s %9s %8s %11s %8s\n", "samples", "window_ms", "mean_ns", "min_ns",
                "mean/min", "max_ns", "abs_ok");
    for (int n : {10'000, 100'000, 500'000, 2'000'000}) {
        const ClockCheck c = clock_selftest(n);
        std::printf("%10d %11.2f %10.1f %9llu %8.2f %11llu %8s\n", n,
                    static_cast<double>(c.elapsed_ns) / 1e6, c.mean_call_ns,
                    (unsigned long long)c.min_call_ns, c.contention_ratio(),
                    (unsigned long long)c.max_call_ns, c.ok ? "yes" : "NO");
    }
    return 0;
}
