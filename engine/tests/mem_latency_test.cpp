// M3.2: the dependent-load pointer chase.
//
// One thing here matters more than everything else: the chase must be a *single* cycle over
// every line of the buffer. If it is not, the chase spends its time in a subset of the
// working set, that subset fits in a smaller cache than the one being measured, and the
// metric reports a number that is wrong by a factor of thirty while looking entirely
// reasonable. That is what the first half of this file is about.
#include "bench/metric.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/mem_latency.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <set>
#include <vector>

using namespace bench;

namespace {

WorkloadContext ctx_for(std::uint64_t ws, int thread_index = 0,
                        const WorkloadOptions* opts = nullptr) {
    return WorkloadContext{.thread_index = thread_index,
                           .thread_count = 1,
                           .working_set_bytes = ws,
                           .seed = 4242,
                           .metric = Metric::mem_latency,
                           .options = opts,
                           .bytes_written = nullptr};
}

// How many disjoint cycles the permutation i -> a[i] decomposes into.
std::size_t cycle_count(const std::vector<std::uint64_t>& a) {
    std::vector<bool> seen(a.size(), false);
    std::size_t cycles = 0;
    for (std::size_t start = 0; start < a.size(); ++start) {
        if (seen[start])
            continue;
        ++cycles;
        for (std::size_t i = start; !seen[i]; i = static_cast<std::size_t>(a[i]))
            seen[i] = true;
    }
    return cycles;
}

std::vector<std::uint64_t> identity(std::size_t n) {
    std::vector<std::uint64_t> a(n);
    std::iota(a.begin(), a.end(), std::uint64_t{0});
    return a;
}

} // namespace

// ---- Sattolo ------------------------------------------------------------------------------

TEST(MemLatency, SattoloAlwaysProducesExactlyOneCycle) {
    for (std::size_t n : {2u, 3u, 5u, 64u, 1000u, 4096u}) {
        for (std::uint64_t seed = 0; seed < 8; ++seed) {
            auto a = identity(n);
            sattolo_shuffle(a, seed);
            // Still a permutation...
            auto sorted = a;
            std::sort(sorted.begin(), sorted.end());
            EXPECT_EQ(sorted, identity(n)) << "n=" << n << " seed=" << seed;
            // ...and exactly one cycle through all of it.
            EXPECT_EQ(cycle_count(a), 1u) << "n=" << n << " seed=" << seed;
        }
    }
}

TEST(MemLatency, SattoloNeverLeavesAFixedPoint) {
    // A fixed point would be a line whose "next" is itself: the chase would stop there and
    // measure L1 latency forever, at any working set.
    auto a = identity(4096);
    sattolo_shuffle(a, 7);
    for (std::size_t i = 0; i < a.size(); ++i)
        EXPECT_NE(a[i], i) << "fixed point at " << i;
}

TEST(MemLatency, FisherYatesWouldNotHaveWorked) {
    // Why the algorithm is named in the source. Fisher-Yates draws from [0, i] rather than
    // [0, i), which is a uniformly random permutation - and a uniformly random permutation
    // of n elements has about ln(n) cycles, so the chase would usually fall into one that
    // is a fraction of the working set.
    //
    // This is a statistical claim, so it is measured over many seeds rather than asserted
    // once: a single Fisher-Yates shuffle *can* come out as one cycle (probability 1/n).
    constexpr std::size_t kN = 4096;
    int fy_single = 0;
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        auto a = identity(kN);
        std::mt19937_64 rng{seed};
        for (std::size_t i = a.size(); i-- > 1;)
            std::swap(a[i], a[static_cast<std::size_t>(rng() % (i + 1))]); // [0, i], inclusive
        if (cycle_count(a) == 1)
            ++fy_single;
    }
    // Expected about 200/4096 << 1. Anything up to a few would be luck; 200 would mean the
    // two algorithms are the same and this test is not testing anything.
    EXPECT_LT(fy_single, 10) << fy_single << " of 200 Fisher-Yates shuffles were single cycles";
}

// ---- the chase as it ends up in memory -------------------------------------------------------

TEST(MemLatency, TheChaseVisitsEveryLineExactlyOncePerLap) {
    MemLatencyWorkload w;
    w.setup(ctx_for(256u << 10)); // 4096 lines
    const std::uint64_t n = w.lines();
    ASSERT_EQ(n, (256u << 10) / 64);

    std::set<std::uint64_t> visited;
    std::uint64_t line = 0;
    for (std::uint64_t step = 0; step < n; ++step) {
        EXPECT_TRUE(visited.insert(line).second)
            << "line " << line << " revisited at step " << step;
        line = w.next_line(line);
    }
    EXPECT_EQ(visited.size(), n) << "the lap did not cover the working set";
    EXPECT_EQ(line, 0u) << "the chase did not close back on its start";
    w.teardown();
}

TEST(MemLatency, EachThreadChasesADifferentOrder) {
    // Separate buffers already, but identical orders would put two threads in lockstep, and
    // loaded latency should be measured under independent traffic.
    MemLatencyWorkload a, b;
    a.setup(ctx_for(64u << 10, /*thread_index=*/0));
    b.setup(ctx_for(64u << 10, /*thread_index=*/1));
    int same = 0;
    for (std::uint64_t i = 0; i < a.lines(); ++i)
        if (a.next_line(i) == b.next_line(i))
            ++same;
    EXPECT_LT(same, static_cast<int>(a.lines()) / 4);
    a.teardown();
    b.teardown();
}

// ---- sizing and accounting ---------------------------------------------------------------

TEST(MemLatency, BufferSizeIsAWholeNumberOfLinesAndNeverBelowACycle) {
    EXPECT_EQ(MemLatencyWorkload::buffer_bytes_for(0), MemLatencyWorkload::kDefaultWorkingSet);
    EXPECT_EQ(MemLatencyWorkload::buffer_bytes_for(4096), 4096u);
    EXPECT_EQ(MemLatencyWorkload::buffer_bytes_for(200), 192u); // 3 lines
    // A chase needs two lines to be a cycle at all.
    EXPECT_EQ(MemLatencyWorkload::buffer_bytes_for(1), 128u);
    EXPECT_EQ(MemLatencyWorkload::buffer_bytes_for(64), 128u);
    EXPECT_EQ(effective_working_set(Workload::mem_latency, 200), 192u);
}

TEST(MemLatency, RunBatchReturnsTheDeclaredBatchUnitsAndAdvancesTheChase) {
    MemLatencyWorkload w;
    w.setup(ctx_for(64u << 10));
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(w.run_batch(), w.batch_units());
    EXPECT_EQ(w.batch_units(), MemLatencyWorkload::kBatchLoads);
    w.teardown();
}

TEST(MemLatency, ColdRegionIsTheWholeChase) {
    MemLatencyWorkload w;
    w.setup(ctx_for(64u << 10));
    EXPECT_EQ(w.cold_region().size(), 64u << 10);
    w.teardown();
}

TEST(MemLatency, TheBuffersOwnHugePageGrantIsReportedSeparately) {
    // params.huge_pages describes the cold-prep scratch buffer. For this workload the chase
    // buffer *is* the measurement, and whether it got huge pages decides whether a large
    // sweep point is measuring cache misses or TLB misses.
    MemLatencyWorkload w;
    w.setup(ctx_for(8u << 20));
    const json d = w.describe();
    EXPECT_TRUE(d["huge_pages_requested"].get<bool>());
    EXPECT_EQ(d["chase_lines"].get<std::uint64_t>(), (8u << 20) / 64);
    EXPECT_FALSE(d["thp_policy"].get<std::string>().empty());
    // Not asserted as granted: the policy may be "never", and the kernel may simply fail to
    // find contiguous memory. What is asserted is that the two are consistent.
    if (d["buffer_huge_pages"].get<bool>()) {
        EXPECT_GT(d["buffer_huge_page_bytes"].get<std::uint64_t>(), 0u);
    }
    w.teardown();
}

TEST(MemLatency, NoHugepagesIsHonoured) {
    WorkloadOptions opts;
    opts.huge_pages = false;
    MemLatencyWorkload w;
    w.setup(ctx_for(8u << 20, 0, &opts));
    EXPECT_FALSE(w.huge_requested());
    w.teardown();
}

// ---- how a latency value is formed ---------------------------------------------------------

TEST(MemLatency, AValueIsNanosecondsPerDependentLoad) {
    EXPECT_EQ(work_unit_of(Metric::mem_latency), WorkUnit::loads);
    EXPECT_EQ(value_rule_of(Metric::mem_latency), ValueRule::ns_per_unit);
    EXPECT_EQ(unit_of(Metric::mem_latency), Unit::ns);
    EXPECT_TRUE(lower_is_better(Metric::mem_latency));
    EXPECT_DOUBLE_EQ(value_from_units(Metric::mem_latency, 1000, 100'000), 100.0);
}

TEST(MemLatency, MoreThreadsDoNotMakeALoadFaster) {
    // The bug this guards against: summing the load counts across threads and dividing wall
    // time by the sum reports latency/N, a "latency" that falls as you add threads. Eight
    // threads each doing 1000 loads in 100 us took 100 ns per load each, not 12.5.
    EXPECT_DOUBLE_EQ(value_from_units(Metric::mem_latency, 8 * 1000, 100'000, 8), 100.0);
    // A rate is the opposite: summing across threads is the whole point.
    EXPECT_DOUBLE_EQ(value_from_units(Metric::cpu_int_ops, 8 * 1000, 1'000'000'000, 8), 8000.0);
}

// ---- the runner's side ------------------------------------------------------------------

TEST(MemLatency, TheRunnerEmitsOneMetricWithTheRightUnitAndWorkingSet) {
    RunConfig c;
    c.workloads = {Workload::mem_latency};
    c.working_sets = {0}; // ask for the default and check it is recorded, not left at 0
    c.trials = 2;
    c.warmup_trials = 1;
    c.warmup_ms = 0;
    c.trial_ms = 5;
    c.spin_ms = 0;
    c.cold = ColdMode::none;

    MachineInfo m;
    m.hostname = "test-host";
    m.cpu_model = "test-cpu";
    m.physical_cores = 4;
    m.logical_cpus = 8;
    m.l3_kb = 24576;
    m.memory_bytes = 1ull << 34;
    m.os = "test-os";
    m.kernel = "test-kernel";
    m.compiler = "g++";
    m.compiler_flags = "-O3";
    m.engine_version = "0.1.0";
    m.engine_git_sha = "test";
    m.id = machine_id(m);

    const auto run = run_benchmarks(c, m, {"bench"});
    ASSERT_EQ(run.summary.size(), 1u);
    EXPECT_EQ(run.summary[0].metric, Metric::mem_latency);
    EXPECT_EQ(run.summary[0].working_set_bytes, MemLatencyWorkload::kDefaultWorkingSet);
    EXPECT_GT(run.summary[0].median, 0.0);
    ASSERT_FALSE(run.results.empty());
    EXPECT_EQ(run.results.front().params["unit_of_work"].get<std::string>(), "loads");
    EXPECT_TRUE(validate_run(to_json(run)).empty());
}
