// Normative enums for workloads, metrics and units. These tables must match
// schema/benchmark-result.schema.json; tests/metric_schema_test.cpp enforces that.
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace bench {

enum class Workload { cpu_int, cpu_fp, cpu_hash, mem_bw, mem_latency, disk_seq, disk_rand };

enum class Metric {
    cpu_int_ops,
    cpu_fp_ops,
    cpu_hash_ops,
    mem_read_bw,
    mem_write_bw,
    mem_copy_bw,
    mem_latency,
    disk_seq_read_bw,
    disk_seq_write_bw,
    disk_rand_read_iops,
    disk_rand_write_iops,
    disk_rand_read_p99_us,
};

enum class Unit { ops_per_s, gb_per_s, ns, mb_per_s, iops, us };

inline constexpr std::array kAllWorkloads = {
    Workload::cpu_int,     Workload::cpu_fp,   Workload::cpu_hash, Workload::mem_bw,
    Workload::mem_latency, Workload::disk_seq, Workload::disk_rand,
};

inline constexpr std::array kAllMetrics = {
    Metric::cpu_int_ops,         Metric::cpu_fp_ops,        Metric::cpu_hash_ops,
    Metric::mem_read_bw,         Metric::mem_write_bw,      Metric::mem_copy_bw,
    Metric::mem_latency,         Metric::disk_seq_read_bw,  Metric::disk_seq_write_bw,
    Metric::disk_rand_read_iops, Metric::disk_rand_write_iops, Metric::disk_rand_read_p99_us,
};

inline constexpr std::array kAllUnits = {
    Unit::ops_per_s, Unit::gb_per_s, Unit::ns, Unit::mb_per_s, Unit::iops, Unit::us,
};

std::string_view to_string(Workload w) noexcept;
std::string_view to_string(Metric m) noexcept;
std::string_view to_string(Unit u) noexcept;

std::optional<Workload> parse_workload(std::string_view s) noexcept;
std::optional<Metric> parse_metric(std::string_view s) noexcept;
std::optional<Unit> parse_unit(std::string_view s) noexcept;

Unit unit_of(Metric m) noexcept;
Workload workload_of(Metric m) noexcept;

// True when lower values are better (latencies).
bool lower_is_better(Metric m) noexcept;

} // namespace bench
