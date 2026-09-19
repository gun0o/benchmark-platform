#include "bench/stats.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <vector>

using namespace bench;

namespace {

// Relative tolerance 1e-9 with an absolute floor for values near zero.
::testing::AssertionResult near_rel(const char* a_expr, const char* b_expr, double a, double b) {
    const double tol = 1e-9 * std::max({1.0, std::fabs(a), std::fabs(b)});
    if (std::fabs(a - b) <= tol)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << a_expr << " = " << a << " vs " << b_expr << " = " << b
                                         << " (diff " << std::fabs(a - b) << ", tol " << tol << ")";
}
#define EXPECT_NEAR_REL(a, b) EXPECT_PRED_FORMAT2(near_rel, a, b)

nlohmann::json load_fixture() {
    std::ifstream f{std::filesystem::path{BENCH_FIXTURE_DIR} / "stats.json"};
    EXPECT_TRUE(f.good()) << "tests/fixtures/stats.json missing (run gen_stats_fixture.py)";
    return nlohmann::json::parse(f);
}

} // namespace

TEST(Stats, MatchesNumpyFixtures) {
    const auto fx = load_fixture();
    ASSERT_GE(fx["cases"].size(), 10u);
    for (const auto& c : fx["cases"]) {
        SCOPED_TRACE(c["name"].get<std::string>());
        const auto values = c["values"].get<std::vector<double>>();
        ASSERT_EQ(values.size(), c["n"].get<std::size_t>());
        const SummaryStats s = summarize(values);
        EXPECT_EQ(s.n, values.size());
        EXPECT_NEAR_REL(s.mean, c["mean"].get<double>());
        EXPECT_NEAR_REL(s.stddev, c["std"].get<double>());
        EXPECT_NEAR_REL(s.median, c["median"].get<double>());
        EXPECT_NEAR_REL(s.p5, c["p5"].get<double>());
        EXPECT_NEAR_REL(s.p95, c["p95"].get<double>());
        EXPECT_NEAR_REL(s.min, c["min"].get<double>());
        EXPECT_NEAR_REL(s.max, c["max"].get<double>());
        EXPECT_NEAR_REL(s.cov, c["cov"].get<double>());
        EXPECT_NEAR_REL(s.mad, c["mad"].get<double>());
        // Free functions agree with summarize().
        EXPECT_NEAR_REL(mean(values), s.mean);
        EXPECT_NEAR_REL(sample_stddev(values), s.stddev);
        EXPECT_NEAR_REL(median(values), s.median);
        EXPECT_NEAR_REL(percentile(values, 95.0), s.p95);
        EXPECT_NEAR_REL(cov(values), s.cov);
        EXPECT_NEAR_REL(mad(values), s.mad);
    }
}

TEST(Stats, WelfordSurvivesCatastrophicCancellation) {
    // Naive sum(x^2) - n*mean^2 loses everything here; Welford must give exactly 1.0.
    Welford w;
    for (double x : {1e9 + 1, 1e9 + 2, 1e9 + 3})
        w.add(x);
    EXPECT_EQ(w.n, 3u);
    EXPECT_DOUBLE_EQ(w.mean, 1e9 + 2);
    EXPECT_DOUBLE_EQ(w.variance(), 1.0);
    EXPECT_DOUBLE_EQ(w.stddev(), 1.0);
}

TEST(Stats, WelfordMergeEqualsSinglePass) {
    const auto fx = load_fixture();
    for (const auto& c : fx["cases"]) {
        SCOPED_TRACE(c["name"].get<std::string>());
        const auto values = c["values"].get<std::vector<double>>();
        Welford all;
        for (double x : values)
            all.add(x);
        // Merging reassociates the floating-point work, so the two results agree only to
        // within the conditioning of the problem: kappa = mean / stddev. For well-behaved
        // data kappa is single digits and this is a tight check; for the deliberately
        // ill-conditioned fixture (values ~1e9, stddev ~1) kappa is ~1e9 and a few parts
        // in 1e6 is all single-pass accumulation can promise.
        const double kappa = all.stddev() > 0 ? 1.0 + std::fabs(all.mean) / all.stddev() : 1.0;
        const double tol = 64 * std::numeric_limits<double>::epsilon() * kappa;
        for (std::size_t split :
             {std::size_t{0}, values.size() / 3, values.size() / 2, values.size()}) {
            Welford a, b;
            for (std::size_t i = 0; i < values.size(); ++i)
                (i < split ? a : b).add(values[i]);
            a.merge(b);
            EXPECT_EQ(a.n, all.n);
            EXPECT_NEAR(a.mean, all.mean, tol * std::fabs(all.mean));
            EXPECT_NEAR(a.variance(), all.variance(), tol * std::fabs(all.variance()));
        }
    }
}

// Documents *why* summarize() is two-pass rather than reusing Welford: on data whose mean
// dwarfs its spread, the single-pass result drifts far outside the 1e-9 tolerance that the
// two-pass result meets comfortably.
TEST(Stats, TwoPassBeatsSinglePassOnIllConditionedData) {
    const auto fx = load_fixture();
    for (const auto& c : fx["cases"]) {
        if (c["name"] != "large_offset_cancellation")
            continue;
        const auto values = c["values"].get<std::vector<double>>();
        const double reference = c["std"].get<double>();
        Welford w;
        for (double x : values)
            w.add(x);
        const double single_pass_err = std::fabs(w.stddev() - reference) / reference;
        const double two_pass_err = std::fabs(sample_stddev(values) - reference) / reference;
        std::printf("[ stats ] stddev of 1e9 + N(0,1), n=%zu: two-pass rel.err %.2e, "
                    "Welford rel.err %.2e\n",
                    values.size(), two_pass_err, single_pass_err);
        EXPECT_LT(two_pass_err, 1e-12) << "two-pass must match numpy essentially exactly";
        EXPECT_GT(single_pass_err, two_pass_err) << "the point of the comparison";
        return;
    }
    FAIL() << "fixture case 'large_offset_cancellation' not found";
}

TEST(Stats, WelfordMergeWithEmptyIsIdentity) {
    Welford a, empty;
    a.add(2.0);
    a.add(4.0);
    const Welford before = a;
    a.merge(empty);
    EXPECT_EQ(a.n, before.n);
    EXPECT_EQ(a.mean, before.mean);
    EXPECT_EQ(a.m2, before.m2);
    empty.merge(a);
    EXPECT_EQ(empty.n, 2u);
    EXPECT_DOUBLE_EQ(empty.mean, 3.0);
    EXPECT_DOUBLE_EQ(empty.variance(), 2.0);
}

TEST(Stats, EdgeCases) {
    EXPECT_EQ(summarize({}).n, 0u);
    const std::vector<double> one{5.0};
    const SummaryStats s1 = summarize(one);
    EXPECT_EQ(s1.n, 1u);
    EXPECT_DOUBLE_EQ(s1.mean, 5.0);
    EXPECT_DOUBLE_EQ(s1.stddev, 0.0) << "n < 2: no sample variance";
    EXPECT_DOUBLE_EQ(s1.median, 5.0);
    EXPECT_DOUBLE_EQ(s1.p5, 5.0);
    EXPECT_DOUBLE_EQ(s1.p95, 5.0);
    const std::vector<double> constant(50, 7.5);
    const SummaryStats sc = summarize(constant);
    EXPECT_DOUBLE_EQ(sc.stddev, 0.0);
    EXPECT_DOUBLE_EQ(sc.cov, 0.0);
    EXPECT_DOUBLE_EQ(sc.mad, 0.0);
    const std::vector<double> zeros(4, 0.0);
    EXPECT_DOUBLE_EQ(cov(zeros), 0.0) << "mean 0: cov defined as 0, not NaN";
    EXPECT_TRUE(std::isnan(percentile({}, 50.0)));
}

TEST(Stats, PercentileIsNumpyLinear) {
    const std::vector<double> v{1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(percentile(v, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(percentile(v, 100.0), 4.0);
    EXPECT_DOUBLE_EQ(percentile(v, 50.0), 2.5);  // rank 1.5 -> between 2 and 3
    EXPECT_DOUBLE_EQ(percentile(v, 25.0), 1.75); // rank 0.75
    EXPECT_DOUBLE_EQ(median(std::vector<double>{3.0, 1.0, 2.0}), 2.0);
}
