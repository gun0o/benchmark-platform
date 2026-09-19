#include "bench/stats.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace bench {

void Welford::add(double x) noexcept {
    ++n;
    const double delta = x - mean;
    mean += delta / static_cast<double>(n);
    m2 += delta * (x - mean); // uses the *updated* mean: that is what keeps it stable
}

void Welford::merge(const Welford& o) noexcept {
    if (o.n == 0)
        return;
    if (n == 0) {
        *this = o;
        return;
    }
    const double na = static_cast<double>(n), nb = static_cast<double>(o.n);
    const double delta = o.mean - mean;
    const double total = na + nb;
    mean += delta * nb / total;
    m2 += o.m2 + delta * delta * na * nb / total;
    n += o.n;
}

double Welford::variance() const noexcept {
    return n < 2 ? 0.0 : m2 / static_cast<double>(n - 1);
}
double Welford::stddev() const noexcept {
    return std::sqrt(variance());
}

double mean(std::span<const double> v) noexcept {
    if (v.empty())
        return 0.0;
    double sum = 0.0;
    for (double x : v)
        sum += x;
    return sum / static_cast<double>(v.size());
}

// Two-pass: subtracting the computed mean cancels the common offset exactly, so the
// squared deviations are computed on values of the right magnitude. This is what numpy
// does, and it is why `summarize` does not reuse Welford: the runner already holds every
// trial value in memory (it needs them sorted for the percentiles anyway).
double sample_variance(std::span<const double> v) noexcept {
    if (v.size() < 2)
        return 0.0;
    const double m = mean(v);
    double sumsq = 0.0;
    for (double x : v) {
        const double d = x - m;
        sumsq += d * d;
    }
    return sumsq / static_cast<double>(v.size() - 1);
}

double sample_stddev(std::span<const double> v) noexcept {
    return std::sqrt(sample_variance(v));
}

namespace {

// numpy 'linear' on an already sorted range.
double percentile_sorted(std::span<const double> sorted, double p) {
    if (sorted.empty())
        return std::nan("");
    const double rank = p / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

std::vector<double> sorted_copy(std::span<const double> v) {
    std::vector<double> s(v.begin(), v.end());
    std::ranges::sort(s);
    return s;
}

} // namespace

double percentile(std::span<const double> v, double p) {
    return percentile_sorted(sorted_copy(v), p);
}

double median(std::span<const double> v) {
    return percentile(v, 50.0);
}

double cov(std::span<const double> v) noexcept {
    const double m = mean(v);
    return m == 0.0 ? 0.0 : sample_stddev(v) / m;
}

double mad(std::span<const double> v) {
    if (v.empty())
        return std::nan("");
    const double m = median(v);
    auto dev = v | std::views::transform([m](double x) { return std::fabs(x - m); });
    std::vector<double> d(dev.begin(), dev.end());
    return median(d);
}

SummaryStats summarize(std::span<const double> v) {
    SummaryStats s;
    if (v.empty())
        return s;
    const std::vector<double> sorted = sorted_copy(v);
    s.n = v.size();
    s.mean = mean(v);
    s.stddev = sample_stddev(v);
    s.cov = s.mean == 0.0 ? 0.0 : s.stddev / s.mean;
    s.min = sorted.front();
    s.max = sorted.back();
    s.median = percentile_sorted(sorted, 50.0);
    s.p5 = percentile_sorted(sorted, 5.0);
    s.p95 = percentile_sorted(sorted, 95.0);
    s.mad = mad(v);
    return s;
}

} // namespace bench
