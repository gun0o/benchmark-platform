// M3.1 timing-ratio checks, labelled `perf`: the claims that only hold if the kernels are
// really touching memory rather than a register or a constant the compiler folded.
//
// These are ratios within one run, never absolute numbers, and the margins are wide: the
// measured L1-to-DRAM read ratio on this laptop is about 11x and the assertion below is 3x.
// A machine where they fail has either a broken kernel or no cache hierarchy to speak of.
// `ctest -LE perf` skips them on a noisy runner.
#include "bench/metric.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workloads/mem_bw.hpp"

#include <gtest/gtest.h>
#include <map>

using namespace bench;

namespace {

// Every claim in this file assumes the kernels are limited by memory. In an unoptimized or
// instrumented build they are limited by their own instruction count instead, and the cache
// hierarchy stops being visible at all: measured in the debug preset, an L1-resident read
// runs at 22.4 GB/s against DRAM's 17.9 GB/s, a ratio of 1.25x where the release build
// gives 11x. That is the build being slow, not the machine having no L1, so asserting a
// ratio there would be asserting the wrong thing.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(__OPTIMIZE__)
#define BENCH_NOT_MEMORY_BOUND 1
#else
#define BENCH_NOT_MEMORY_BOUND 0
#endif

// median GB/s per working set for one metric, from a single run so the power and thermal
// state is shared between the points being compared.
std::map<std::uint64_t, double> sweep(Metric m, std::vector<std::uint64_t> working_sets) {
    RunConfig c;
    c.metrics = {m};
    c.working_sets = std::move(working_sets);
    c.trials = 5;
    c.warmup_trials = 2;
    c.warmup_ms = 50;
    c.trial_ms = 20;
    c.spin_ms = 100;
    c.seed = 99;
    c.cold = ColdMode::none;
    const auto run = run_benchmarks(c, collect_machine_info(), {"bench"});
    std::map<std::uint64_t, double> out;
    for (const auto& s : run.summary)
        out[s.working_set_bytes] = s.median;
    return out;
}

} // namespace

TEST(MemBwScaling, ReadingFromL1IsFarFasterThanReadingFromDram) {
    if (BENCH_NOT_MEMORY_BOUND)
        GTEST_SKIP() << "this build is instruction-bound; see the note above";
    const auto s = sweep(Metric::mem_read_bw, {32u << 10, 256u << 20});
    ASSERT_EQ(s.size(), 2u);
    const double l1 = s.at(32u << 10), dram = s.at(256u << 20);
    EXPECT_GT(l1, 3.0 * dram) << "L1 " << l1 << " GB/s vs DRAM " << dram << " GB/s";
}

TEST(MemBwScaling, WritingLeavesL1AtACliffBecauseOfWriteAllocate) {
    if (BENCH_NOT_MEMORY_BOUND)
        GTEST_SKIP() << "this build is instruction-bound; see the note above";
    // A store to a line that is not resident has to fetch the line first, so the moment the
    // buffer stops fitting in L1d the write loop starts paying for a read it never asked
    // for. The cliff is much sharper than the read curve's - measured 3.6x here against
    // the read curve's 1.3x - which is the write-allocate cost made visible.
    const auto r = sweep(Metric::mem_read_bw, {32u << 10, 256u << 10});
    const auto w = sweep(Metric::mem_write_bw, {32u << 10, 256u << 10});
    ASSERT_EQ(r.size(), 2u);
    ASSERT_EQ(w.size(), 2u);
    const double r_drop = r.at(32u << 10) / r.at(256u << 10);
    const double w_drop = w.at(32u << 10) / w.at(256u << 10);
    EXPECT_GT(w_drop, r_drop) << "write drop " << w_drop << "x vs read drop " << r_drop << "x";
    EXPECT_GT(w_drop, 1.5);
}

TEST(MemBwScaling, MoreThreadsBuyMoreDramBandwidthUntilTheControllerIsTheLimit) {
    if (BENCH_NOT_MEMORY_BOUND)
        GTEST_SKIP() << "this build is instruction-bound; see the note above";
    RunConfig c;
    c.metrics = {Metric::mem_read_bw};
    c.thread_counts = {1, 4};
    c.working_sets = {64u << 20};
    c.trials = 5;
    c.warmup_trials = 2;
    c.warmup_ms = 50;
    c.trial_ms = 20;
    c.spin_ms = 100;
    c.cold = ColdMode::none;
    const auto run = run_benchmarks(c, collect_machine_info(), {"bench"});
    ASSERT_EQ(run.summary.size(), 2u);
    const double one = run.summary[0].median, four = run.summary[1].median;
    // Aggregate goes up (a single core cannot keep enough misses in flight to saturate
    // DRAM) but nowhere near 4x (the controller can).
    EXPECT_GT(four, 1.3 * one) << one << " -> " << four << " GB/s";
    EXPECT_LT(four, 4.0 * one) << one << " -> " << four << " GB/s";
}
