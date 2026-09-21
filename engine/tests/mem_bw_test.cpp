// M3.1: the streaming bandwidth kernels, and the runner machinery that made room for a
// workload with more than one metric.
//
// What is tested here is that each kernel moves the bytes its metric claims it moves, that
// the byte count it reports is the number it actually moved, and that a result says which
// of the three it was. Whether the machine is fast is measured, not asserted; the
// cache-hierarchy ratio checks live in mem_bw_scaling_test.cpp (labelled perf).
#include "bench/metric.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/mem_bw.hpp"

#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

using namespace bench;

namespace {

WorkloadContext ctx_for(Metric m, std::uint64_t ws, const WorkloadOptions* opts = nullptr) {
    return WorkloadContext{.thread_index = 0,
                           .thread_count = 1,
                           .working_set_bytes = ws,
                           .seed = 1234,
                           .metric = m,
                           .options = opts,
                           .bytes_written = nullptr};
}

// A machine block that passes validate_run(): a default-constructed MachineInfo has an
// empty id and would be rejected for that, not for anything the runner did.
MachineInfo test_machine() {
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
    return m;
}

std::vector<std::uint64_t> words_of(std::span<const std::byte> s) {
    std::vector<std::uint64_t> out(s.size() / sizeof(std::uint64_t));
    std::memcpy(out.data(), s.data(), out.size() * sizeof(std::uint64_t));
    return out;
}

} // namespace

// ---- sizing -----------------------------------------------------------------------------

TEST(MemBw, BufferSizeIsAWholeNumberOfCacheLines) {
    EXPECT_EQ(MemBwWorkload::buffer_bytes_for(0), MemBwWorkload::kDefaultWorkingSet);
    EXPECT_EQ(MemBwWorkload::buffer_bytes_for(64), 64u);
    EXPECT_EQ(MemBwWorkload::buffer_bytes_for(100), 64u); // rounds down
    EXPECT_EQ(MemBwWorkload::buffer_bytes_for(1u << 20), 1u << 20);
    // Never zero: a one-byte request still gets one line to walk.
    EXPECT_EQ(MemBwWorkload::buffer_bytes_for(1), 64u);
}

TEST(MemBw, TheRecordedWorkingSetIsTheOneThatWasActuallyTouched) {
    // effective_working_set is what the runner puts in working_set_bytes, so a result can
    // never claim a size the workload rounded away from.
    EXPECT_EQ(effective_working_set(Workload::mem_bw, 100), 64u);
    EXPECT_EQ(effective_working_set(Workload::mem_bw, 0), MemBwWorkload::kDefaultWorkingSet);
    // cpu_* are deliberately left alone; see workload.hpp.
    EXPECT_EQ(effective_working_set(Workload::cpu_int, 0), 0u);
    EXPECT_EQ(effective_working_set(Workload::cpu_hash, 100), 100u);
}

TEST(MemBw, ABatchIsAWholeNumberOfPassesAndNeverAShortOne) {
    // A single pass over a small buffer takes about as long as three clock reads, so the
    // batch repeats passes until it is worth timing. Big buffers do exactly one.
    for (std::uint64_t ws : {4096u, 1u << 16, 1u << 20}) {
        MemBwWorkload w;
        w.setup(ctx_for(Metric::mem_read_bw, ws));
        EXPECT_GE(w.passes_per_batch() * ws, MemBwWorkload::kMinBatchBytes) << "ws=" << ws;
        EXPECT_EQ(w.batch_units(), w.passes_per_batch() * ws) << "ws=" << ws;
        w.teardown();
    }
    MemBwWorkload big;
    big.setup(ctx_for(Metric::mem_read_bw, 64u << 20));
    EXPECT_EQ(big.passes_per_batch(), 1u);
    big.teardown();
}

TEST(MemBw, RunBatchReturnsTheDeclaredBatchUnits) {
    for (Metric m : {Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw}) {
        MemBwWorkload w;
        w.setup(ctx_for(m, 1u << 18));
        for (int i = 0; i < 3; ++i)
            EXPECT_EQ(w.run_batch(), w.batch_units()) << to_string(m);
        w.teardown();
    }
}

// ---- what each kernel actually does ------------------------------------------------------

TEST(MemBw, ModeFollowsTheMetric) {
    EXPECT_EQ(MemBwWorkload::mode_of(Metric::mem_read_bw), MemBwWorkload::Mode::read);
    EXPECT_EQ(MemBwWorkload::mode_of(Metric::mem_write_bw), MemBwWorkload::Mode::write);
    EXPECT_EQ(MemBwWorkload::mode_of(Metric::mem_copy_bw), MemBwWorkload::Mode::copy);
}

TEST(MemBw, ReadLeavesTheBufferAloneAndItsChecksumIsTheSumOfTheWords) {
    MemBwWorkload w;
    w.setup(ctx_for(Metric::mem_read_bw, 1u << 16));
    const auto before = words_of(w.source());
    std::uint64_t expect = 0;
    for (std::uint64_t v : before)
        expect += v;

    const std::uint64_t before_digest = w.digest();
    w.run_batch();
    EXPECT_EQ(words_of(w.source()), before) << "a read kernel must not write";
    // One batch adds the whole-buffer sum once per pass. Unsigned wraparound is the
    // intended arithmetic here, which is why the check is an equality and not a bound.
    EXPECT_EQ(w.digest(), before_digest + expect * w.passes_per_batch());
    w.teardown();
}

TEST(MemBw, WriteFillsEveryWordOfTheBuffer) {
    MemBwWorkload w;
    w.setup(ctx_for(Metric::mem_write_bw, 1u << 16));
    w.run_batch();
    const auto after = words_of(w.source());
    ASSERT_FALSE(after.empty());
    for (std::uint64_t v : after)
        EXPECT_EQ(v, after.front()) << "the whole buffer must hold one runtime value";
    // ...and that value is not zero, i.e. the fill really ran rather than leaving the
    // allocation's zeros in place.
    EXPECT_NE(after.front(), 0u);
    w.teardown();
}

TEST(MemBw, CopyMakesTheDestinationEqualTheSource) {
    MemBwWorkload w;
    w.setup(ctx_for(Metric::mem_copy_bw, 1u << 16));
    ASSERT_EQ(w.source().size(), w.destination().size());
    ASSERT_NE(words_of(w.source()), words_of(w.destination()));
    w.run_batch();
    EXPECT_EQ(words_of(w.destination()), words_of(w.source()));
    // Bytes are counted once, not read + written: a batch moves passes x working_set.
    EXPECT_EQ(w.batch_units(), w.passes_per_batch() * w.buffer_bytes());
    w.teardown();
}

TEST(MemBw, NonTemporalStoresProduceTheSameBytesAsPlainOnes) {
    // The NT path is a different instruction sequence with its own tail handling and its
    // own fence. It has to be the same memcpy/memset, or --nt would be measuring something
    // else while claiming to measure the same metric.
    if (!MemBwWorkload::non_temporal_available())
        GTEST_SKIP() << "this build has no 32-byte non-temporal stores";
    WorkloadOptions nt;
    nt.non_temporal = true;
    for (Metric m : {Metric::mem_write_bw, Metric::mem_copy_bw}) {
        MemBwWorkload plain, stream;
        plain.setup(ctx_for(m, (1u << 16) + 64)); // + 64: exercise the non-multiple-of-32 tail
        stream.setup(ctx_for(m, (1u << 16) + 64, &nt));
        EXPECT_FALSE(plain.non_temporal());
        EXPECT_TRUE(stream.non_temporal());
        plain.run_batch();
        stream.run_batch();
        EXPECT_EQ(words_of(plain.destination()), words_of(stream.destination()))
            << to_string(m) << ": --nt changed the result, not just how it was stored";
        plain.teardown();
        stream.teardown();
    }
}

TEST(MemBw, NonTemporalIsNeverClaimedForReads) {
    WorkloadOptions nt;
    nt.non_temporal = true;
    MemBwWorkload w;
    w.setup(ctx_for(Metric::mem_read_bw, 1u << 16, &nt));
    EXPECT_FALSE(w.non_temporal()) << "there is no such thing as a non-temporal load here";
    EXPECT_FALSE(w.describe()["nt_effective"].get<bool>());
    EXPECT_TRUE(w.describe()["nt"].get<bool>()) << "...but the request is still recorded";
    w.teardown();
}

TEST(MemBw, ColdRegionCoversEverythingATrialTouches) {
    MemBwWorkload r, c;
    r.setup(ctx_for(Metric::mem_read_bw, 1u << 16));
    c.setup(ctx_for(Metric::mem_copy_bw, 1u << 16));
    EXPECT_EQ(r.cold_region().size(), 1u << 16);
    EXPECT_EQ(c.cold_region().size(), 2u << 16) << "copy must cool its source and destination";
    r.teardown();
    c.teardown();
}

// ---- how a value is formed ----------------------------------------------------------------

TEST(MemBw, BandwidthMetricsCountBytesAndReportGigabytesPerSecond) {
    for (Metric m : {Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw}) {
        EXPECT_EQ(work_unit_of(m), WorkUnit::bytes);
        EXPECT_EQ(value_rule_of(m), ValueRule::giga_units_per_s);
        EXPECT_EQ(unit_of(m), Unit::gb_per_s);
        // 1 GB is 1e9 bytes, not 2^30: 2e9 bytes in one second is 2.0, not 1.86.
        EXPECT_DOUBLE_EQ(value_from_units(m, 2'000'000'000ull, 1'000'000'000ull), 2.0);
    }
}

TEST(MemBw, AValueIsAlwaysFiniteEvenWhenNothingHappened) {
    EXPECT_DOUBLE_EQ(value_from_units(Metric::mem_read_bw, 0, 1000), 0.0);
    EXPECT_DOUBLE_EQ(value_from_units(Metric::mem_read_bw, 1000, 0), 0.0);
}

// ---- the runner's side of a multi-metric workload -------------------------------------------

namespace {

RunConfig quick_mem(Metric m, std::uint64_t ws) {
    RunConfig c;
    c.metrics = {m};
    c.working_sets = {ws};
    c.trials = 2;
    c.warmup_trials = 1;
    c.warmup_ms = 0;
    c.trial_ms = 5;
    c.spin_ms = 0;
    c.seed = 7;
    c.cold = ColdMode::none;
    return c;
}

} // namespace

TEST(MemBw, OneWorkloadFlagRunsAllThreeMetricsAsSeparateConfigurations) {
    RunConfig c = quick_mem(Metric::mem_read_bw, 1u << 16);
    c.metrics.clear();
    c.workloads = {Workload::mem_bw};
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});

    std::set<Metric> seen;
    for (const auto& s : run.summary) {
        seen.insert(s.metric);
        EXPECT_EQ(s.workload, Workload::mem_bw);
        EXPECT_EQ(s.n, 2);
        EXPECT_GT(s.mean, 0.0);
    }
    EXPECT_EQ(seen,
              (std::set<Metric>{Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw}));
    // Three configurations, two trials each: the metrics do not share a trial. Sharing one
    // would mean a trial that read *and* wrote, which is neither metric.
    EXPECT_EQ(run.results.size(), 6u);
}

TEST(MemBw, ResultsCarryTheUnitAndTheUnitOfWork) {
    const auto run =
        run_benchmarks(quick_mem(Metric::mem_read_bw, 1u << 16), test_machine(), {"bench"});
    ASSERT_FALSE(run.results.empty());
    for (const auto& r : run.results) {
        EXPECT_EQ(r.metric, Metric::mem_read_bw);
        EXPECT_EQ(r.working_set_bytes, 1u << 16);
        EXPECT_EQ(r.params["unit_of_work"].get<std::string>(), "bytes");
        EXPECT_EQ(r.params["mem_mode"].get<std::string>(), "read");
        // ops is a byte count here, and a whole number of batches of it.
        const auto ops = r.params["ops"].get<std::uint64_t>();
        const auto batch = r.params["batch_ops"].get<std::uint64_t>();
        EXPECT_GT(batch, 0u);
        EXPECT_EQ(ops % batch, 0u);
    }
    // The run document must be emittable: metric <-> unit <-> workload consistency is what
    // validate_run checks, and it is the same check `bench validate` runs on a file.
    EXPECT_TRUE(validate_run(to_json(run)).empty());
}

TEST(MemBw, AZeroWorkingSetIsRecordedAsTheDefaultItActuallyUsed) {
    RunConfig c = quick_mem(Metric::mem_read_bw, 0);
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    ASSERT_FALSE(run.results.empty());
    EXPECT_EQ(run.results.front().working_set_bytes, MemBwWorkload::kDefaultWorkingSet);
}

TEST(MemBw, RiderMetricsNeverGetAConfigurationOfTheirOwn) {
    // mem_bw has no riders; disk_rand_read_p99_us is the one that does. Expanding a
    // workload into metrics must leave riders out either way, or they would be run twice.
    EXPECT_EQ(metrics_of(Workload::mem_bw).size(), 3u);
    EXPECT_TRUE(riders_of(Metric::mem_read_bw).empty());
    EXPECT_FALSE(is_rider(Metric::mem_read_bw));
}
