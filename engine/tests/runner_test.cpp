#include "bench/runner.hpp"
#include "bench/stats.hpp"
#include "bench/sysinfo.hpp"
#include "bench/timing.hpp"

#include <gtest/gtest.h>

using namespace bench;

// A machine block that passes validate_run(); the default-constructed MachineInfo has an
// empty id and would be rejected for that, not for anything the runner did.
static MachineInfo test_machine() {
    MachineInfo m;
    m.hostname = "test-host";
    m.cpu_model = "test-cpu";
    m.physical_cores = 4;
    m.logical_cpus = 8;
    m.l1d_kb = 48;
    m.l2_kb = 2048;
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

static RunConfig quick(int trials, int warmup, int trial_ms) {
    RunConfig c;
    c.workloads = {Workload::cpu_int};
    c.trials = trials;
    c.warmup_trials = warmup;
    c.warmup_ms = 0; // tests control warmup by count only
    c.trial_ms = trial_ms;
    c.spin_ms = 0;
    c.seed = 42;
    return c;
}

TEST(Runner, WarmupTrialsAreRunButNotEmitted) {
    int seen_warmup = 0, seen_timed = 0;
    const auto run =
        run_benchmarks(quick(2, 3, 5), MachineInfo{}, {"bench"},
                       [&](const Result&, bool warmup) { (warmup ? seen_warmup : seen_timed)++; });
    EXPECT_EQ(seen_warmup, 3);
    EXPECT_EQ(seen_timed, 2);
    ASSERT_EQ(run.results.size(), 2u);
    EXPECT_EQ(run.results[0].trial, 0);
    EXPECT_EQ(run.results[1].trial, 1);
    EXPECT_EQ(run.results[0].params["warmup_trials"], 3);
}

TEST(Runner, TrialsAreTimeBoxedToWholeBatches) {
    const auto run = run_benchmarks(quick(3, 0, 20), MachineInfo{}, {"bench"});
    for (const auto& r : run.results) {
        EXPECT_GE(r.duration_ns, 20'000'000u) << "trial ended before trial_ms";
        // Overshoot is at most one batch (~1M ops, well under 5 ms on any modern core).
        EXPECT_LT(r.duration_ns, 40'000'000u) << "trial overshot by more than a batch";
        const auto ops = r.params["ops"].get<std::uint64_t>();
        const auto batch_ops = r.params["batch_ops"].get<std::uint64_t>();
        EXPECT_GT(ops / batch_ops, 1u);
        EXPECT_EQ(ops % batch_ops, 0u) << "ops must be a whole number of batches";
        EXPECT_NEAR(r.value, static_cast<double>(ops) / (static_cast<double>(r.duration_ns) / 1e9),
                    1e-6);
        EXPECT_GT(r.value, 1e7) << "even a debug build should exceed 10M ops/s";
    }
}

TEST(Runner, OpsScaleWithTrialLength) {
    // Both runs use the warmup time floor so neither measures inside the settling window
    // that follows spawning a pool (see M2.1 notes); otherwise the shorter run is biased low.
    // Nine trials, not three: single-thread throughput on this machine varies by ~10%
    // per trial, so a three-trial mean has a standard error large enough to make any
    // tight bound flaky. Medians below also keep one preempted trial from deciding it.
    RunConfig ca = quick(9, 2, 30), cb = quick(9, 2, 60);
    ca.warmup_ms = cb.warmup_ms = 300;
    const auto a = run_benchmarks(ca, MachineInfo{}, {"bench"});
    const auto b = run_benchmarks(cb, MachineInfo{}, {"bench"});
    auto median_of = [](const RunEnvelope& r, auto&& field) {
        std::vector<double> v;
        for (const auto& x : r.results)
            v.push_back(field(x));
        return median(v);
    };
    const double ops_ratio = median_of(b, [](const Result& x) {
                                 return static_cast<double>(x.params["ops"].get<std::uint64_t>());
                             }) /
                             median_of(a, [](const Result& x) {
                                 return static_cast<double>(x.params["ops"].get<std::uint64_t>());
                             });
    const double rate_a = median_of(a, [](const Result& x) { return x.value; });
    const double rate_b = median_of(b, [](const Result& x) { return x.value; });

    // The invariant that proves the work is real: a longer trial does proportionally more
    // work, so the *rate* is unchanged. Comparing ops against a literal 2.0 instead would
    // fold in the trial's overshoot, which is up to one whole batch and is a large
    // fraction of a short trial under a sanitizer.
    EXPECT_GT(ops_ratio, 1.5) << "a longer trial must do more work";
    EXPECT_NEAR(rate_b, rate_a, 0.20 * rate_a)
        << "ops per second changed with trial length: work is being skipped or added";
}

TEST(Runner, WarmupTimeFloorAddsWarmupTrials) {
    RunConfig c = quick(2, 1, 5);
    c.warmup_ms =
        60; // several trials of 5 ms (plus per-trial overhead) before the first timed trial
    int seen_warmup = 0;
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"},
                                    [&](const Result&, bool warmup) { seen_warmup += warmup; });
    EXPECT_GT(seen_warmup, c.warmup_trials) << "the time floor must add trials beyond the count";
    EXPECT_GE(seen_warmup, 4) << "60 ms floor at 5 ms trials, even with 10 ms overhead each";
    ASSERT_EQ(run.results.size(), 2u);
    EXPECT_EQ(run.results[0].params["warmups_run"], seen_warmup);
}

TEST(Runner, RejectsBadConfig) {
    EXPECT_THROW(run_benchmarks(quick(0, 0, 5), MachineInfo{}, {"bench"}), std::invalid_argument);
    EXPECT_THROW(run_benchmarks(quick(1, -1, 5), MachineInfo{}, {"bench"}), std::invalid_argument);
    EXPECT_THROW(run_benchmarks(quick(1, 0, 0), MachineInfo{}, {"bench"}), std::invalid_argument);
}

TEST(Runner, UnimplementedWorkloadThrows) {
    RunConfig c = quick(1, 0, 5);
    c.workloads = {Workload::cpu_fp};
    EXPECT_THROW(run_benchmarks(c, MachineInfo{}, {"bench"}), std::runtime_error);
}

// ---- M2.1: worker pool -----------------------------------------------------------------

#include <thread>

TEST(Pool, DefaultCpuListIsStrideTwoThenOdds) {
    EXPECT_EQ(default_cpu_list(6), (std::vector<int>{0, 2, 4, 1, 3, 5}));
    EXPECT_EQ(default_cpu_list(1), (std::vector<int>{0}));
    EXPECT_EQ(default_cpu_list(22).size(), 22u);
}

TEST(Pool, AggregateIsSumOfThreadsOverWallTime) {
    RunConfig c = quick(3, 1, 20);
    c.thread_counts = {4};
    c.warmup_ms = 300; // past the pool's settling window, so the thread windows overlap properly
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    ASSERT_EQ(run.results.size(), 3u);
    for (const auto& r : run.results) {
        EXPECT_EQ(r.thread_count, 4);
        const auto per = r.params["per_thread_ops_per_s"];
        ASSERT_EQ(per.size(), 4u);
        for (const auto& v : per)
            EXPECT_GT(v.get<double>(), 1e7);
        // Aggregate over wall time can only be <= the sum of per-thread rates (each
        // thread's own window is a subset of release..done).
        double sum = 0;
        for (const auto& v : per)
            sum += v.get<double>();
        EXPECT_LE(r.value, sum * 1.001);
        EXPECT_GT(r.value, sum * 0.5) << "aggregate collapsed: threads not overlapping?";
        EXPECT_GE(r.duration_ns, 20'000'000u);
        // Functional bound only (one trial length); the tight bound is measured by
        // barrier_sync_test.
        EXPECT_LT(r.params["start_spread_us"].get<double>(), 20000.0);
    }
}

TEST(Pool, PinningKeepsWorkersOnAssignedCpus) {
    const int n = static_cast<int>(std::min(4u, std::thread::hardware_concurrency()));
    RunConfig c = quick(5, 1, 10);
    c.thread_counts = {n};
    c.pin = true;
    c.cpus = default_cpu_list(static_cast<int>(std::thread::hardware_concurrency()));
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    for (const auto& r : run.results) {
        EXPECT_EQ(r.params["pin_failures"], 0);
        EXPECT_EQ(r.params["pin_violations"], 0);
        const auto cpus = r.params["cpus"].get<std::vector<int>>();
        const auto seen = r.params["per_thread_cpu"].get<std::vector<int>>();
        ASSERT_EQ(cpus.size(), seen.size());
        for (std::size_t i = 0; i < cpus.size(); ++i)
            EXPECT_EQ(seen[i], cpus[i]);
    }
}

TEST(Pool, AbortStopsAtTrialBoundary) {
    RunConfig c = quick(50, 0, 5);
    c.thread_counts = {2};
    int seen = 0;
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"}, [&](const Result&, bool) {
        if (++seen == 3)
            request_abort();
    });
    clear_abort();
    EXPECT_EQ(run.results.size(), 3u) << "should stop after the trial in which abort was requested";
}

// ---- M2.2: per-config summaries and the safety cap ---------------------------------------

#include "bench/stats.hpp"

#include <cmath>

TEST(Summary, OnePerConfigurationOverTimedTrialsOnly) {
    RunConfig c = quick(5, 2, 5);
    c.thread_counts = {1, 2};
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    ASSERT_EQ(run.summary.size(), 2u) << "one summary per (workload, threads, working set)";
    ASSERT_EQ(run.results.size(), 10u);
    for (const auto& s : run.summary) {
        EXPECT_EQ(s.workload, Workload::cpu_int);
        EXPECT_EQ(s.metric, Metric::cpu_int_ops);
        EXPECT_EQ(s.n, 5) << "warmup trials must not be in the summary";
        EXPECT_EQ(s.working_set_bytes, 0u);
    }
    EXPECT_EQ(run.summary[0].thread_count, 1);
    EXPECT_EQ(run.summary[1].thread_count, 2);
}

TEST(Summary, MatchesStatsOverTheEmittedTrialValues) {
    RunConfig c = quick(8, 1, 5);
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    ASSERT_EQ(run.summary.size(), 1u);
    std::vector<double> values;
    for (const auto& r : run.results)
        values.push_back(r.value);
    const SummaryStats expected = summarize(values);
    const Summary& s = run.summary[0];
    EXPECT_EQ(static_cast<std::size_t>(s.n), values.size());
    EXPECT_DOUBLE_EQ(s.mean, expected.mean);
    EXPECT_DOUBLE_EQ(s.median, expected.median);
    EXPECT_DOUBLE_EQ(s.stddev, expected.stddev);
    EXPECT_DOUBLE_EQ(s.cov, expected.cov);
    EXPECT_DOUBLE_EQ(s.min, expected.min);
    EXPECT_DOUBLE_EQ(s.p5, expected.p5);
    EXPECT_DOUBLE_EQ(s.p95, expected.p95);
    EXPECT_DOUBLE_EQ(s.max, expected.max);
    EXPECT_DOUBLE_EQ(s.mad, expected.mad);
    // Sanity: ordering holds and cov is a plausible fraction.
    EXPECT_LE(s.min, s.median);
    EXPECT_LE(s.median, s.max);
    EXPECT_GE(s.cov, 0.0);
    EXPECT_LT(s.cov, 1.0);
}

TEST(Summary, SurvivesJsonRoundTripAndValidates) {
    const auto run = run_benchmarks(quick(4, 1, 5), test_machine(), {"bench"});
    const json j = to_json(run);
    ASSERT_EQ(j["summary"].size(), 1u);
    EXPECT_TRUE(j["summary"][0].contains("mad"));
    EXPECT_TRUE(validate_run(j).empty());
    const Summary back = summary_from_json(j["summary"][0]);
    EXPECT_EQ(back, run.summary[0]);
}

TEST(Summary, ValidationCatchesImpossibleSummaries) {
    const auto run = run_benchmarks(quick(4, 1, 5), test_machine(), {"bench"});
    json j = to_json(run);
    j["summary"][0]["min"] = j["summary"][0]["max"].get<double>() + 1.0;
    EXPECT_FALSE(validate_run(j).empty()) << "min > max must be rejected";

    j = to_json(run);
    j["summary"][0]["stddev"] = -1.0;
    EXPECT_FALSE(validate_run(j).empty()) << "negative stddev must be rejected";

    j = to_json(run);
    j["summary"][0]["metric"] = "mem_latency"; // workload stays cpu_int
    EXPECT_FALSE(validate_run(j).empty()) << "metric/workload mismatch must be rejected";

    j = to_json(run);
    j["summary"][0].erase("cov");
    EXPECT_FALSE(validate_run(j).empty()) << "missing cov must be rejected";
}

TEST(Runner, MaxSecondsStopsTheRun) {
    RunConfig c = quick(1000, 0, 5); // would take ~5 s without the cap
    c.max_seconds = 0.4;
    const Timer t;
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    const double elapsed = t.elapsed_s();
    EXPECT_LT(elapsed, 3.0) << "the cap should have stopped the run long before 1000 trials";
    EXPECT_GT(run.results.size(), 0u) << "trials completed before the cap are kept";
    EXPECT_LT(run.results.size(), 1000u);
    ASSERT_EQ(run.summary.size(), 1u) << "a partial config still gets a summary";
    EXPECT_EQ(static_cast<std::size_t>(run.summary[0].n), run.results.size())
        << "the summary covers exactly the trials that ran";
    EXPECT_TRUE(validate_run(to_json(run)).empty());
}

TEST(Runner, MaxSecondsSkipsLaterConfigurations) {
    RunConfig c = quick(200, 0, 5);
    c.thread_counts = {1, 2, 4};
    c.max_seconds = 0.3;
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    EXPECT_LT(run.summary.size(), 3u) << "remaining thread counts must not start";
    for (const auto& r : run.results)
        EXPECT_EQ(r.thread_count, 1) << "the cap hit during the first configuration";
}

TEST(Runner, RejectsBadMaxSeconds) {
    RunConfig c = quick(1, 0, 5);
    c.max_seconds = -1.0;
    EXPECT_THROW(run_benchmarks(c, MachineInfo{}, {"bench"}), std::invalid_argument);
    c.max_seconds = std::nan("");
    EXPECT_THROW(run_benchmarks(c, MachineInfo{}, {"bench"}), std::invalid_argument);
}
