// M3.2 timing-ratio checks, labelled `perf`.
//
// Only the claims that survive this machine's hybrid topology are asserted here. The
// L2->L3 step is *not*, even though the milestone's whole point is the step pattern:
// measured over 8 repetitions, an 8 MiB chase came out at 17.0, 17.2, 18.6, 19.3, 29.0,
// 35.9, 107.5 and 129.9 ns - bimodal, because under WSL2 a thread can land on a core with
// a very different path to the last-level cache and nothing in the guest says which. A
// test that asserted "8 MiB is faster than DRAM" would fail a quarter of the time on a
// correct engine. The distribution is reported in docs/notes/M3.2.md instead, which is
// where a fact that cannot be asserted belongs.
//
// What is asserted is the part with two orders of magnitude of headroom.
#include "bench/metric.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workloads/mem_latency.hpp"

#include <gtest/gtest.h>
#include <map>

using namespace bench;

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(__OPTIMIZE__)
#define BENCH_NOT_MEMORY_BOUND 1
#else
#define BENCH_NOT_MEMORY_BOUND 0
#endif

std::map<std::uint64_t, double> sweep(std::vector<std::uint64_t> working_sets) {
    RunConfig c;
    c.metrics = {Metric::mem_latency};
    c.working_sets = std::move(working_sets);
    c.trials = 5;
    c.warmup_trials = 2;
    c.warmup_ms = 50;
    c.trial_ms = 20;
    c.spin_ms = 100;
    c.seed = 5;
    c.cold = ColdMode::none;
    const auto run = run_benchmarks(c, collect_machine_info(), {"bench"});
    std::map<std::uint64_t, double> out;
    for (const auto& s : run.summary)
        out[s.working_set_bytes] = s.median;
    return out;
}

} // namespace

TEST(MemLatencyScaling, AnL1ResidentChaseIsOrdersOfMagnitudeFasterThanADramOne) {
    if (BENCH_NOT_MEMORY_BOUND)
        GTEST_SKIP() << "an unoptimized or instrumented build is bound by its own loop";
    const auto s = sweep({32u << 10, 128u << 20});
    ASSERT_EQ(s.size(), 2u);
    const double l1 = s.at(32u << 10), dram = s.at(128u << 20);
    // Measured ~1.0 ns against ~155 ns, a factor of 150. Asserting 10 leaves room for a
    // machine whose memory is much closer than this one's, or whose L1 is slower.
    EXPECT_GT(dram, 10.0 * l1) << "L1 " << l1 << " ns vs DRAM " << dram << " ns";
    // And an L1-resident dependent load is a few cycles, not a few hundred.
    EXPECT_LT(l1, 10.0) << "L1 chase at " << l1 << " ns/load";
}

TEST(MemLatencyScaling, TheChaseDefeatsThePrefetcherThatMakesStreamingFast) {
    if (BENCH_NOT_MEMORY_BOUND)
        GTEST_SKIP() << "an unoptimized or instrumented build is bound by its own loop";
    // The reason both mem_bw and mem_latency exist. Over the same DRAM-sized working set, a
    // streaming read moves a 64-byte line in far less time than a dependent load takes to
    // fetch one, because streaming loads overlap and chased loads cannot.
    RunConfig c;
    c.metrics = {Metric::mem_read_bw};
    c.working_sets = {128u << 20};
    c.trials = 5;
    c.warmup_trials = 2;
    c.warmup_ms = 50;
    c.trial_ms = 20;
    c.spin_ms = 100;
    c.cold = ColdMode::none;
    const auto bw = run_benchmarks(c, collect_machine_info(), {"bench"});
    ASSERT_EQ(bw.summary.size(), 1u);
    const double ns_per_line_streaming = 64.0 / bw.summary[0].median; // GB/s -> ns per 64 B

    const double ns_per_line_chased = sweep({128u << 20}).at(128u << 20);
    EXPECT_GT(ns_per_line_chased, 5.0 * ns_per_line_streaming)
        << "chased " << ns_per_line_chased << " ns/line vs streamed " << ns_per_line_streaming
        << " ns/line";
}
