// Does cold preparation actually cool the cache? The only honest way to answer is to time
// something whose speed depends entirely on where the data is, so this test builds a random
// cyclic pointer chase: every load's address comes from the previous load's result, so the
// CPU can neither prefetch it (the pattern is random) nor overlap the loads (each depends on
// the one before). The time per step is then a direct read-out of the level of the memory
// hierarchy the data is sitting in.
//
// Warm, a 256 KiB chase lives in L2 and costs single-digit nanoseconds per step. Cold, every
// step is a DRAM round trip, 80-100 ns. That gap is the measurement.
//
// Labelled `perf`: it asserts timing ratios, so `ctest -LE perf` skips it on noisy runners.
#include "bench/cache.hpp"
#include "bench/stats.hpp"
#include "bench/timing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <gtest/gtest.h>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

using namespace bench;

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BENCH_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define BENCH_SANITIZED 1
#endif
#endif

// Not pinned, deliberately. This test reads the memory hierarchy, and on a hybrid part the
// hierarchy depends on which core type you land on, so pinning looks like the obvious fix.
// Measured, it is not: pinning the measuring thread to vCPU 0 made the spread worse (the
// warm baseline itself went from 3.65 to 12.34 ns/load in one run of three, and the control
// test's ratio drifted to 1.30x). vCPU 0 also carries interrupt and kernel work, and M2.1
// already established that guest-level pinning does not determine the physical core under
// Hyper-V. Left unpinned, the fix for the spread is repetitions, which is what kReps does.
constexpr std::size_t kChaseBytes = 256 << 10; // fits L2 (2 MiB/core here), far exceeds L1d
constexpr std::size_t kSteps = kChaseBytes / kCacheLine;
// 41, not 9. An explicit flush lands on DRAM every single time, but eviction depends on
// the cache's replacement policy choosing to discard the chase, and that is a distribution:
// measured over 60 repetitions its per-repetition ratio ranged from 1.1x to over 30x. Nine
// samples of that produce a median that swings between 3x and 24x run to run, which is a
// test that fails for reasons that have nothing to do with the code.
constexpr int kReps = 41; // odd, so the median is an actual measurement

// One pointer per 64-byte line, arranged into a single cycle through all of them by
// Sattolo's algorithm. Sattolo (unlike Fisher-Yates) guarantees exactly one cycle of full
// length, so a chase of `kSteps` visits every line exactly once and returns to the start.
// Without that guarantee the chase could fall into a short sub-cycle and stay in L1.
void build_chase(std::span<std::byte> buf, std::uint64_t seed) {
    const std::size_t n = buf.size() / kCacheLine;
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937_64 rng{seed};
    for (std::size_t i = n - 1; i > 0; --i) { // Sattolo: j < i, never j == i
        const std::size_t j = rng() % i;
        std::swap(order[i], order[j]);
    }
    for (std::size_t i = 0; i < n; ++i) {
        auto* slot = reinterpret_cast<void**>(buf.data() + order[i] * kCacheLine);
        *slot = buf.data() + order[(i + 1) % n] * kCacheLine;
    }
}

// Returns nanoseconds per dependent load.
double chase_ns_per_step(std::span<std::byte> buf, std::size_t steps) {
    void* p = buf.data();
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < steps; ++i)
        p = *reinterpret_cast<void**>(p);
    const auto t1 = Clock::now();
    DoNotOptimize(p); // without this the whole loop is dead code
    return static_cast<double>(ns_between(t0, t1)) / static_cast<double>(steps);
}

struct Ab {
    std::vector<double> warm; // ns/step with the data already in cache, one per repetition
    std::vector<double> cold; // ns/step on the first pass after cooling
    std::vector<double> ratio_per_rep;

    [[nodiscard]] double ratio() const { return median(cold) / median(warm); }
};

// `cool` is whatever is under test; it runs between a warming pass and the timed pass.
template <class Cool> Ab measure(Cool cool) {
    // The same 500 ms frequency ramp the runner does before its first trial. Without it the
    // first measurements are taken at an idle clock and the numbers wander by 3x.
    busy_spin(std::chrono::milliseconds(500));
    Buffer buf = alloc_buffer(kChaseBytes, AllocOptions{.huge_pages = false});
    build_chase(buf.span(), 20260919);

    Ab ab;
    ab.warm.reserve(kReps);
    ab.cold.reserve(kReps);
    ab.ratio_per_rep.reserve(kReps);
    for (int r = 0; r < kReps + 1; ++r) {
        // Two warming passes: the first faults in nothing (already touched) but pulls the
        // whole cycle into L2, the second measures it already there.
        chase_ns_per_step(buf.span(), kSteps);
        const double w = chase_ns_per_step(buf.span(), kSteps);
        cool(buf.span());
        const double c = chase_ns_per_step(buf.span(), kSteps);
        if (r == 0)
            continue; // discard the first repetition: frequency is still ramping
        ab.warm.push_back(w);
        ab.cold.push_back(c);
        ab.ratio_per_rep.push_back(c / w);
    }
    return ab;
}

// maybe_unused: under a sanitizer every test body below is replaced by GTEST_SKIP, so
// nothing calls this and -Werror=unused-function would fail the build.
[[maybe_unused]] void report(const char* what, const Ab& ab, double want) {
    // The per-repetition spread is reported, not just the median: how *reliably* a mechanism
    // cools the cache is the property that decides which one should be the default.
    std::cerr << std::format(
        "{:<18} n={:<3} warm {:6.2f}  cold p5 {:6.2f} / med {:7.2f} / p95 {:7.2f} ns/load   "
        "ratio {:6.2f}x (per-rep {:5.2f}x..{:6.2f}x)  require >= {:.1f}x\n",
        what, ab.warm.size(), median(ab.warm), percentile(ab.cold, 5), median(ab.cold),
        percentile(ab.cold, 95), ab.ratio(), *std::ranges::min_element(ab.ratio_per_rep),
        *std::ranges::max_element(ab.ratio_per_rep), want);
}

} // namespace

// Verify 1: clflush makes a warm 256 KiB region cold. Expect DRAM (80-100 ns) against L2
// (~4 ns), so >= 10x; assert >= 5x so VM scheduling noise cannot fail a correct flush.
TEST(ColdCache, FlushMakesTheNextPassMissToDram) {
#ifdef BENCH_SANITIZED
    GTEST_SKIP() << "timing ratios are meaningless under a sanitizer";
#else
    if (!has_clflushopt())
        std::cerr << "note: CLFLUSHOPT unavailable, falling back to CLFLUSH\n";
    const Ab ab = measure([](std::span<std::byte> b) { flush_lines(b); });
    report("clflush 256 KiB", ab, 5.0);
    EXPECT_GT(median(ab.warm), 0.0);
    EXPECT_GE(ab.ratio(), 5.0) << "the flush did not cool the region";
#endif
}

// Verify 2: filling the cache with something else works too, just more bluntly. It is a
// weaker effect than an explicit flush (the chase can keep a share of the cache, and the
// pass is not precise about what it displaces), so the bar is 3x.
TEST(ColdCache, EvictionMakesTheNextPassMissToDram) {
#ifdef BENCH_SANITIZED
    GTEST_SKIP() << "timing ratios are meaningless under a sanitizer";
#else
    Buffer scratch = alloc_buffer(evict_buffer_bytes(24576)); // 2 x 24 MiB L3
    const Ab ab = measure([&scratch](std::span<std::byte>) { evict_llc(scratch.span()); });
    report("evict 48 MiB", ab, 3.0);
    EXPECT_GE(ab.ratio(), 3.0) << "streaming 2x L3 did not displace the region";
#endif
}

// The control: with no cooling at all the two passes must be indistinguishable. Without
// this, a chase that was simply slow every time would pass both tests above.
TEST(ColdCache, NoColdModeLeavesTheRegionWarm) {
#ifdef BENCH_SANITIZED
    GTEST_SKIP() << "timing ratios are meaningless under a sanitizer";
#else
    const Ab ab = measure([](std::span<std::byte>) {});
    report("none (control)", ab, 0.0);
    EXPECT_LT(ab.ratio(), 1.5) << "a pass with no cooling must stay warm";
#endif
}

// Verify 3: what `--cold none` versus `--cold clflush` looks like over a run of trials.
// PLAN.md specifies this on mem_latency at a 1 MiB working set; mem_latency is M3.2, so the
// same experiment runs here on the pointer chase this file already has, which is the kernel
// mem_latency will be built from.
//
// The claim being checked has two halves, and they pull in opposite directions:
//   - cold mode must cost something on the first pass, or it is not doing anything;
//   - it must cost nothing once the data is back, or it would be changing the measurement
//     rather than controlling its starting point.
// Steady state is not the second pass: after a full flush the second pass is still paying
// for lines the first pass could not keep. It settles by the third.
TEST(ColdCache, ColdCostsTheFirstPassAndNothingAfterward) {
#ifdef BENCH_SANITIZED
    GTEST_SKIP() << "timing ratios are meaningless under a sanitizer";
#else
    constexpr int kTrials = 12;
    constexpr int kPasses = 5;
    busy_spin(std::chrono::milliseconds(500));

    std::cerr << "ns per dependent load, median of 10 trials after 2 discarded:\n"
              << "  size     warm  |  after flush: p1      p2     p3     p4     p5\n";
    struct Row {
        std::size_t kb;
        double warm;
        double pass[kPasses];
    };
    std::vector<Row> rows;
    for (const std::size_t kb : {std::size_t{64}, std::size_t{256}, std::size_t{1024}}) {
        Buffer buf = alloc_buffer(kb << 10, AllocOptions{.huge_pages = false});
        build_chase(buf.span(), 20260919);
        const std::size_t steps = buf.size() / kCacheLine;

        std::vector<double> warm;
        std::vector<double> passes[kPasses];
        for (int t = 0; t < kTrials; ++t) {
            chase_ns_per_step(buf.span(), steps); // --cold none: the region stays warm
            chase_ns_per_step(buf.span(), steps);
            const double w = chase_ns_per_step(buf.span(), steps);
            flush_lines(buf.span()); // --cold clflush
            double p[kPasses];
            for (int k = 0; k < kPasses; ++k)
                p[k] = chase_ns_per_step(buf.span(), steps);
            if (t < 2)
                continue; // discard while the frequency settles
            warm.push_back(w);
            for (int k = 0; k < kPasses; ++k)
                passes[k].push_back(p[k]);
        }
        Row row{kb, median(warm), {}};
        for (int k = 0; k < kPasses; ++k)
            row.pass[k] = median(passes[k]);
        std::cerr << std::format("  {:4} KiB {:6.2f}  |             {:7.2f} {:6.2f} {:6.2f} "
                                 "{:6.2f} {:6.2f}\n",
                                 row.kb, row.warm, row.pass[0], row.pass[1], row.pass[2],
                                 row.pass[3], row.pass[4]);
        rows.push_back(row);
    }

    for (const Row& row : rows) {
        EXPECT_GT(row.pass[0], 5.0 * row.warm)
            << row.kb << " KiB: the flush cost the first pass nothing";
        EXPECT_LT(row.pass[kPasses - 1], 1.3 * row.warm)
            << row.kb << " KiB: steady state never came back to the warm value";
    }
#endif
}
