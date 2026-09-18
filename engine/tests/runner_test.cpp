#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"

#include <gtest/gtest.h>

using namespace bench;

static RunConfig quick(int trials, int warmup, int trial_ms) {
    RunConfig c;
    c.workloads = {Workload::cpu_int};
    c.trials = trials;
    c.warmup_trials = warmup;
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
        const auto batches = r.params["batches"].get<std::uint64_t>();
        const auto batch_ops = r.params["batch_ops"].get<std::uint64_t>();
        EXPECT_GT(batches, 1u);
        EXPECT_EQ(ops, batches * batch_ops) << "ops must be a whole number of batches";
        EXPECT_NEAR(r.value, static_cast<double>(ops) / (static_cast<double>(r.duration_ns) / 1e9),
                    1e-6);
        EXPECT_GT(r.value, 1e7) << "even a debug build should exceed 10M ops/s";
    }
}

TEST(Runner, OpsScaleWithTrialLength) {
    const auto a = run_benchmarks(quick(3, 2, 30), MachineInfo{}, {"bench"});
    const auto b = run_benchmarks(quick(3, 2, 60), MachineInfo{}, {"bench"});
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
