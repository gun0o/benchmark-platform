#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"

#include <gtest/gtest.h>

using namespace bench;

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
    RunConfig ca = quick(3, 2, 30), cb = quick(3, 2, 60);
    ca.warmup_ms = cb.warmup_ms = 300;
    const auto a = run_benchmarks(ca, MachineInfo{}, {"bench"});
    const auto b = run_benchmarks(cb, MachineInfo{}, {"bench"});
    auto mean_ops = [](const RunEnvelope& r) {
        double s = 0;
        for (const auto& x : r.results)
            s += static_cast<double>(x.params["ops"].get<std::uint64_t>());
        return s / static_cast<double>(r.results.size());
    };
    const double ratio = mean_ops(b) / mean_ops(a);
    EXPECT_GT(ratio, 1.6) << "doubling trial_ms should ~double ops";
    EXPECT_LT(ratio, 2.4);
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
