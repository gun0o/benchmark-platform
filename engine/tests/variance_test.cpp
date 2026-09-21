// M2.5: the machinery the variance study needs — interleaved configurations, the late-trial
// count, and the clock-canary re-run rule. The variance *numbers* are measured and recorded
// in docs/results/m2.5/; what is tested here is that the mechanisms do what they claim.
#include "bench/result.hpp"
#include "bench/runner.hpp"
#include "bench/stats.hpp"
#include "bench/sysinfo.hpp"
#include "bench/timing.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

using namespace bench;

// The engine's own constant is internal to runner.cpp; the tests only need a large one.
static constexpr int kCanaryMaxSamplesForTest = 2'000'000;

namespace {

MachineInfo test_machine() {
    MachineInfo m;
    m.hostname = "test-host";
    m.cpu_model = "test-cpu";
    m.physical_cores = 4;
    m.logical_cpus = 8;
    m.l3_kb = 24576;
    m.os = "test-os";
    m.kernel = "test-kernel";
    m.compiler = "g++";
    m.compiler_flags = "-O3";
    m.engine_version = "0.1.0";
    m.engine_git_sha = "test";
    m.id = machine_id(m);
    return m;
}

RunConfig two_configs(int trials, int trial_ms) {
    RunConfig c;
    c.workloads = {Workload::cpu_int, Workload::cpu_fp};
    c.thread_counts = {1};
    c.trials = trials;
    c.warmup_trials = 1;
    c.warmup_ms = 0;
    c.trial_ms = trial_ms;
    c.spin_ms = 0;
    c.seed = 42;
    c.canary_retries = 0; // the canary is tested separately; keep these runs deterministic
    return c;
}

// The order configurations produced trials in, as "metric@threads" strings.
std::vector<std::string> trial_order(const RunConfig& c, const MachineInfo& m) {
    std::vector<std::string> order;
    run_benchmarks(c, m, {"bench"}, [&](const Result& r, bool warmup) {
        if (!warmup)
            order.push_back(std::string{to_string(r.metric)} + "@" +
                            std::to_string(r.thread_count));
    });
    return order;
}

} // namespace

// ---- interleaving ------------------------------------------------------------------------

TEST(Interleave, SequentialRunsEachConfigToCompletion) {
    RunConfig c = two_configs(4, 5);
    const auto order = trial_order(c, MachineInfo{});
    ASSERT_EQ(order.size(), 8u);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(order[static_cast<std::size_t>(i)], "cpu_int_ops@1") << "position " << i;
    for (int i = 4; i < 8; ++i)
        EXPECT_EQ(order[static_cast<std::size_t>(i)], "cpu_fp_ops@1") << "position " << i;
}

TEST(Interleave, InterleavedRunsRotateTrialByTrial) {
    RunConfig c = two_configs(4, 5);
    c.interleave = true;
    const auto order = trial_order(c, MachineInfo{});
    ASSERT_EQ(order.size(), 8u);
    // This is the whole point of the flag: a drift lasting a few trials must land on both
    // configurations, not on whichever one happened to be running at the time.
    for (std::size_t i = 0; i < order.size(); ++i)
        EXPECT_EQ(order[i], i % 2 == 0 ? "cpu_int_ops@1" : "cpu_fp_ops@1") << "position " << i;
}

TEST(Interleave, ProducesTheSameConfigurationsAndTrialCounts) {
    RunConfig seq = two_configs(5, 5);
    RunConfig inter = two_configs(5, 5);
    inter.interleave = true;
    const auto a = run_benchmarks(seq, test_machine(), {"bench"});
    const auto b = run_benchmarks(inter, test_machine(), {"bench"});

    ASSERT_EQ(a.summary.size(), b.summary.size());
    ASSERT_EQ(a.results.size(), b.results.size());
    for (std::size_t i = 0; i < a.summary.size(); ++i) {
        EXPECT_EQ(a.summary[i].workload, b.summary[i].workload);
        EXPECT_EQ(a.summary[i].metric, b.summary[i].metric);
        EXPECT_EQ(a.summary[i].thread_count, b.summary[i].thread_count);
        EXPECT_EQ(a.summary[i].n, b.summary[i].n);
    }
    EXPECT_TRUE(validate_run(to_json(b)).empty());
}

TEST(Interleave, ResultsStayGroupedByConfigurationInTheEnvelope) {
    // Trials are produced in rotation but written grouped, so a run file is still ordered
    // the way every downstream consumer (report, ingest, the dashboard) expects.
    RunConfig c = two_configs(4, 5);
    c.interleave = true;
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    ASSERT_EQ(run.results.size(), 8u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(run.results[i].metric, Metric::cpu_int_ops);
        EXPECT_EQ(run.results[i].trial, static_cast<int>(i)) << "trial indices restart per config";
    }
    for (std::size_t i = 4; i < 8; ++i) {
        EXPECT_EQ(run.results[i].metric, Metric::cpu_fp_ops);
        EXPECT_EQ(run.results[i].trial, static_cast<int>(i - 4));
    }
}

TEST(Interleave, IsRecordedInParams) {
    RunConfig c = two_configs(2, 5);
    for (bool on : {false, true}) {
        c.interleave = on;
        const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
        ASSERT_FALSE(run.results.empty());
        for (const auto& r : run.results)
            EXPECT_EQ(r.params["interleaved"].get<bool>(), on);
    }
}

TEST(Interleave, WarmupFloorCountsThisConfigsOwnTrialTimeNotWallClock) {
    // The floor is "this configuration has been running for at least warmup_ms". Under
    // interleaving most of the wall clock belongs to the *other* configuration, so a
    // wall-clock floor would be satisfied without warming anything up.
    RunConfig c = two_configs(2, 10);
    c.interleave = true;
    c.warmup_trials = 0;
    c.warmup_ms = 80; // ~8 trials of 10 ms of this config's own time
    std::map<std::string, int> warmups;
    run_benchmarks(c, MachineInfo{}, {"bench"}, [&](const Result& r, bool warmup) {
        if (warmup)
            warmups[std::string{to_string(r.metric)}]++;
    });
    ASSERT_EQ(warmups.size(), 2u);
    for (const auto& [metric, n] : warmups)
        EXPECT_GE(n, 6) << metric << " warmed for less than its own floor";
}

// ---- late trials -------------------------------------------------------------------------

TEST(LateTrials, AreCountedPerConfigurationAndNeverExceedN) {
    RunConfig c = two_configs(6, 5);
    c.thread_counts = {2};
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    ASSERT_FALSE(run.summary.empty());
    for (const auto& s : run.summary) {
        EXPECT_GE(s.late_trials, 0);
        EXPECT_LE(s.late_trials, s.n) << "late_trials counts a subset of the timed trials";
    }
    // params.late_start must agree with the threshold the summary counts against.
    for (const auto& r : run.results) {
        const bool late = r.params["late_start"].get<bool>();
        EXPECT_EQ(late, r.params["start_spread_us"].get<double>() > 1000.0);
    }
    // ...and the two must agree with each other.
    for (const auto& s : run.summary) {
        int counted = 0;
        for (const auto& r : run.results)
            if (r.metric == s.metric && r.thread_count == s.thread_count &&
                r.params["late_start"].get<bool>())
                ++counted;
        EXPECT_EQ(counted, s.late_trials) << to_string(s.metric);
    }
}

TEST(LateTrials, AreNotDroppedFromTheSummary) {
    // The pitfall this guards: a late worker is host preemption, and the temptation is to
    // discard the trial. n must always equal the number of timed trials that ran.
    RunConfig c = two_configs(7, 5);
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    for (const auto& s : run.summary)
        EXPECT_EQ(s.n, 7) << "a trial went missing from the summary";
}

// ---- clock canary ------------------------------------------------------------------------

TEST(Canary, RecordsAClockReadingAtBothEndsOfEachConfiguration) {
    RunConfig c = two_configs(3, 5);
    c.canary_retries = 3;
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    ASSERT_FALSE(run.summary.empty());
    for (const auto& s : run.summary) {
        EXPECT_GT(s.clock_call_ns_start, 0.0);
        EXPECT_GT(s.clock_call_ns_end, 0.0);
        EXPECT_GE(s.canary_attempts, 1);
    }
}

TEST(Canary, RetriesAreDisabledByZeroAndTheConfigStillRuns) {
    RunConfig c = two_configs(3, 5);
    c.canary_retries = 0;
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    ASSERT_EQ(run.summary.size(), 2u);
    for (const auto& s : run.summary) {
        EXPECT_EQ(s.canary_attempts, 1) << "no retries were allowed";
        EXPECT_EQ(s.n, 3) << "the configuration must still produce its trials";
    }
}

TEST(Canary, InterleavedRunsRecordButDoNotRetry) {
    RunConfig c = two_configs(3, 5);
    c.interleave = true;
    c.canary_retries = 3;
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    for (const auto& s : run.summary)
        EXPECT_EQ(s.canary_attempts, 1)
            << "re-running one configuration is meaningless when they are braided together";
}

TEST(Canary, SamplingStopsAtTheTimeWindowNotTheSampleCount) {
    // The window has to be a time, not a count: the same count takes 8 ms in a release
    // build and most of a second under a sanitizer, where it was measured consuming a
    // whole --max-seconds budget before a single trial ran.
    const Timer t;
    const ClockCheck c = clock_selftest(100'000'000, std::chrono::milliseconds{8});
    const double elapsed_ms = t.elapsed_s() * 1e3;
    EXPECT_LT(elapsed_ms, 200.0) << "the time cap did not stop a 100M-sample request";
    EXPECT_LT(c.samples, 100'000'000u) << "it should have stopped early";
    EXPECT_GT(c.samples, 0u);
    EXPECT_GT(c.elapsed_ns, 0u);
}

TEST(Canary, SampleCountStillCapsWhenTheWindowIsGenerous) {
    const ClockCheck c = clock_selftest(5000, std::chrono::seconds{10});
    EXPECT_EQ(c.samples, 5000u) << "the count is the other half of the bound";
}

TEST(Canary, ZeroWindowMeansCountOnly) {
    const ClockCheck c = clock_selftest(5000);
    EXPECT_EQ(c.samples, 5000u);
}

TEST(Canary, ContentionRatioIsNearOneOnAQuietHostInAnyBuild) {
    // This is the property the whole rule rests on: mean/min does not care how expensive a
    // now() call is, only whether some calls cost far more than others. It therefore reads
    // the same on a release build (~14 ns/call) and a sanitizer build (~144 ns/call), which
    // an absolute nanosecond threshold does not.
    const ClockCheck c = clock_selftest(kCanaryMaxSamplesForTest, std::chrono::milliseconds{8});
    ASSERT_GT(c.min_call_ns, 0u) << "a clock with 0 ns between calls cannot be reasoned about";
    EXPECT_LE(c.min_call_ns, static_cast<std::uint64_t>(c.mean_call_ns) + 1)
        << "the minimum cannot exceed the mean";
    EXPECT_LE(c.max_call_ns, c.min_call_ns * 1'000'000u);
    // Deliberately loose: this runs on whatever machine the suite runs on, possibly a
    // shared CI box. The rule trips at 3; a quiet host measures ~1.1. Anything under 50
    // still demonstrates the ratio is a ratio and not a scale.
    EXPECT_GT(c.contention_ratio(), 0.0);
    EXPECT_LT(c.contention_ratio(), 50.0);
}

TEST(Canary, ContentionRatioDefaultsToOneWhenTheClockIsTooCoarseToMeasure) {
    ClockCheck c;
    c.mean_call_ns = 500.0;
    c.min_call_ns = 0; // a clock whose resolution swallowed every delta
    EXPECT_DOUBLE_EQ(c.contention_ratio(), 1.0) << "must not divide by zero";
}

// ---- schema -------------------------------------------------------------------------------

TEST(VarianceSchema, NewSummaryFieldsRoundTripAndValidate) {
    RunConfig c = two_configs(3, 5);
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    const json j = to_json(run);
    ASSERT_FALSE(j["summary"].empty());
    for (const char* k :
         {"late_trials", "canary_attempts", "clock_call_ns_start", "clock_call_ns_end"})
        EXPECT_TRUE(j["summary"][0].contains(k)) << k;
    EXPECT_TRUE(validate_run(j).empty());
    EXPECT_EQ(summary_from_json(j["summary"][0]), run.summary[0]);
}

TEST(VarianceSchema, OlderDocumentsWithoutTheNewFieldsStillParse) {
    RunConfig c = two_configs(3, 5);
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    json j = to_json(run);
    for (const char* k :
         {"late_trials", "canary_attempts", "clock_call_ns_start", "clock_call_ns_end"})
        j["summary"][0].erase(k);
    EXPECT_TRUE(validate_run(j).empty()) << "the new fields are optional";
    const Summary back = summary_from_json(j["summary"][0]);
    EXPECT_EQ(back.late_trials, 0);
    EXPECT_EQ(back.canary_attempts, 1) << "a document that does not say defaults to one attempt";
}

TEST(VarianceSchema, ValidationCatchesImpossibleRunQualityFields) {
    RunConfig c = two_configs(3, 5);
    const auto run = run_benchmarks(c, test_machine(), {"bench"});

    json j = to_json(run);
    j["summary"][0]["late_trials"] = j["summary"][0]["n"].get<int>() + 1;
    EXPECT_FALSE(validate_run(j).empty()) << "more late trials than trials must be rejected";

    j = to_json(run);
    j["summary"][0]["late_trials"] = -1;
    EXPECT_FALSE(validate_run(j).empty());

    j = to_json(run);
    j["summary"][0]["canary_attempts"] = 0;
    EXPECT_FALSE(validate_run(j).empty()) << "a summary exists, so at least one attempt ran";

    j = to_json(run);
    j["summary"][0]["clock_call_ns_start"] = -1.0;
    EXPECT_FALSE(validate_run(j).empty());
}

// ---- the knobs the variance study varies ---------------------------------------------------

TEST(Variance, CovIsComputedOverTheRawTimedTrials) {
    // The headline guard for the whole milestone: no trimming, ever. cov must equal
    // stats.hpp's cov over exactly the emitted values.
    RunConfig c = two_configs(9, 5);
    const auto run = run_benchmarks(c, test_machine(), {"bench"});
    for (const auto& s : run.summary) {
        std::vector<double> v;
        for (const auto& r : run.results)
            if (r.metric == s.metric && r.thread_count == s.thread_count)
                v.push_back(r.value);
        ASSERT_EQ(v.size(), static_cast<std::size_t>(s.n));
        EXPECT_DOUBLE_EQ(s.cov, cov(v));
        EXPECT_DOUBLE_EQ(s.stddev, sample_stddev(v));
        EXPECT_DOUBLE_EQ(s.mad, mad(v));
    }
}
