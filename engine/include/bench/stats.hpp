// Descriptive statistics over trial values. Definitions are pinned to numpy so the
// fixtures in tests/fixtures/stats.json are the reference:
//   stddev    = sample standard deviation (ddof = 1). 0 for n < 2.
//   percentile= numpy "linear" interpolation: rank = p/100 * (n-1), interpolate between
//               the two nearest order statistics.
//   median    = percentile(50).
//   cov       = stddev / mean (coefficient of variation, dimensionless).
//   mad       = median(|x - median(x)|), a robust spread measure reported next to cov.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bench {

// Welford's online algorithm: single-pass mean and variance with a merge (Chan, Golub &
// LeVeque) so per-thread or per-chunk accumulators can be combined. Far better than the
// textbook sum(x^2) - n*mean^2, but still a single pass: its relative error grows with the
// condition number kappa = mean / stddev, reaching ~1e-8 when kappa ~ 1e9. Use it when the
// values cannot all be held (streaming, merging); use the batch functions below, which are
// two-pass, whenever the values are in memory. The runner has them in memory.
struct Welford {
    std::uint64_t n = 0;
    double mean = 0.0;
    double m2 = 0.0; // sum of squared deviations from the running mean

    void add(double x) noexcept;
    void merge(const Welford& other) noexcept;
    [[nodiscard]] double variance() const noexcept; // sample variance, 0 for n < 2
    [[nodiscard]] double stddev() const noexcept;
};

// Batch statistics: two-pass (mean first, then squared deviations from it), matching
// numpy's np.mean / np.std(ddof=1) to within a few ulp even when mean >> stddev.
double mean(std::span<const double> v) noexcept;
double sample_variance(std::span<const double> v) noexcept;
double sample_stddev(std::span<const double> v) noexcept;
double percentile(std::span<const double> v, double p); // p in [0, 100]; copies and sorts
double median(std::span<const double> v);
double cov(std::span<const double> v) noexcept; // 0 when mean == 0
double mad(std::span<const double> v);

struct SummaryStats {
    std::uint64_t n = 0;
    double mean = 0, median = 0, stddev = 0, cov = 0, min = 0, p5 = 0, p95 = 0, max = 0, mad = 0;
};

// One sorted copy, one Welford pass. Empty input yields n = 0 and zeros.
SummaryStats summarize(std::span<const double> v);

} // namespace bench
