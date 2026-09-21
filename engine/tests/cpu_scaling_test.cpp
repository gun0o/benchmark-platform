// M2.4 verify: the M1.2 iteration-scaling check applied to all three CPU kernels.
//
// The check: run the same configuration with two different trial lengths. A longer trial
// must do proportionally more work while reporting the same rate. If a kernel were being
// optimized away, or if the runner were estimating ops instead of counting them, the two
// would not track. Labelled `perf` because it compares timings and so needs a machine that
// is not being fought over.
#include "bench/runner.hpp"
#include "bench/stats.hpp"

#include <format>
#include <gtest/gtest.h>
#include <vector>

using namespace bench;

namespace {

RunConfig scaling_cfg(Workload w, int trial_ms) {
    RunConfig c;
    c.workloads = {w};
    c.trials = 9; // medians of 9: one preempted trial must not decide the test (M1.2 note)
    c.warmup_trials = 2;
    c.warmup_ms = 300; // past the pool's settling window
    c.trial_ms = trial_ms;
    c.spin_ms = 0;
    c.seed = 42;
    return c;
}

double median_ops(const RunEnvelope& r) {
    std::vector<double> v;
    for (const auto& x : r.results)
        v.push_back(static_cast<double>(x.params["ops"].get<std::uint64_t>()));
    return median(v);
}

double median_rate(const RunEnvelope& r) {
    std::vector<double> v;
    for (const auto& x : r.results)
        v.push_back(x.value);
    return median(v);
}

void check_scaling(Workload w) {
    const auto a = run_benchmarks(scaling_cfg(w, 30), MachineInfo{}, {"bench"});
    const auto b = run_benchmarks(scaling_cfg(w, 60), MachineInfo{}, {"bench"});
    const double ops_ratio = median_ops(b) / median_ops(a);
    const double rate_a = median_rate(a), rate_b = median_rate(b);
    const std::string name{to_string(w)};
    EXPECT_GT(ops_ratio, 1.5) << name << ": a longer trial must do more work";
    EXPECT_LT(ops_ratio, 2.5) << name << ": more work than the extra time can account for";
    EXPECT_NEAR(rate_b, rate_a, 0.20 * rate_a)
        << name << ": ops/s changed with trial length, so work is being skipped or added";
    // Not an assertion, a record: the interesting number for the write-up.
    std::cerr << std::format("{:<9} 30ms {:.3e} ops/s   60ms {:.3e} ops/s   ops ratio {:.3f}\n",
                             name, rate_a, rate_b, ops_ratio);
}

} // namespace

TEST(CpuScaling, CpuIntOpsScaleWithTrialLength) {
    check_scaling(Workload::cpu_int);
}
TEST(CpuScaling, CpuFpOpsScaleWithTrialLength) {
    check_scaling(Workload::cpu_fp);
}
TEST(CpuScaling, CpuHashOpsScaleWithTrialLength) {
    check_scaling(Workload::cpu_hash);
}

// Target #1 (>= 10M ops/s) is recorded in docs/results, not decided by a test: a green test
// suite must not depend on the host being fast. The floor is still worth asserting as a
// smoke test - but only in a build where the number means anything.
//
// Under ASan it does not. Measured: cpu_hash runs at 1.19e8 ops/s in release and 8.0e6
// under ASan, a 15x slowdown, because every one of the eight loads per block goes through
// a shadow-memory check. That is ASan working correctly, not the kernel being slow, and an
// assertion that fails there is asserting the wrong thing. TSan instruments the same loads.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define BENCH_SANITIZED 1
#else
#define BENCH_SANITIZED 0
#endif

TEST(CpuScaling, EachKernelClearsTheTenMillionOpsFloorOnOneThread) {
    if (BENCH_SANITIZED)
        GTEST_SKIP() << "sanitizer build: throughput here measures the instrumentation";
    for (Workload w : {Workload::cpu_int, Workload::cpu_fp, Workload::cpu_hash}) {
        const auto run = run_benchmarks(scaling_cfg(w, 30), MachineInfo{}, {"bench"});
        ASSERT_EQ(run.summary.size(), 1u);
        EXPECT_GT(run.summary[0].median, 1e7) << to_string(w);
    }
}
