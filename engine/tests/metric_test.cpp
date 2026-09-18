#include "bench/metric.hpp"

#include <gtest/gtest.h>

using namespace bench;

TEST(Metric, RoundTripsThroughStrings) {
    for (auto w : kAllWorkloads)
        EXPECT_EQ(parse_workload(to_string(w)), w);
    for (auto m : kAllMetrics)
        EXPECT_EQ(parse_metric(to_string(m)), m);
    for (auto u : kAllUnits)
        EXPECT_EQ(parse_unit(to_string(u)), u);
}

TEST(Metric, RejectsUnknownNames) {
    EXPECT_FALSE(parse_workload("cpu").has_value());
    EXPECT_FALSE(parse_metric("").has_value());
    EXPECT_FALSE(parse_unit("gb/s").has_value()); // case-sensitive
}

TEST(Metric, TwelveMetricsSevenWorkloads) {
    EXPECT_EQ(kAllMetrics.size(), 12u);
    EXPECT_EQ(kAllWorkloads.size(), 7u);
}

TEST(Metric, UnitAndWorkloadMapping) {
    EXPECT_EQ(unit_of(Metric::cpu_int_ops), Unit::ops_per_s);
    EXPECT_EQ(unit_of(Metric::mem_latency), Unit::ns);
    EXPECT_EQ(unit_of(Metric::disk_rand_read_p99_us), Unit::us);
    EXPECT_EQ(workload_of(Metric::mem_copy_bw), Workload::mem_bw);
    EXPECT_EQ(workload_of(Metric::disk_rand_write_iops), Workload::disk_rand);
    EXPECT_TRUE(lower_is_better(Metric::mem_latency));
    EXPECT_FALSE(lower_is_better(Metric::cpu_fp_ops));
}
