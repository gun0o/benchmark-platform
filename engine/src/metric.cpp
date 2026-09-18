#include "bench/metric.hpp"

namespace bench {

std::string_view to_string(Workload w) noexcept {
    switch (w) {
    case Workload::cpu_int:
        return "cpu_int";
    case Workload::cpu_fp:
        return "cpu_fp";
    case Workload::cpu_hash:
        return "cpu_hash";
    case Workload::mem_bw:
        return "mem_bw";
    case Workload::mem_latency:
        return "mem_latency";
    case Workload::disk_seq:
        return "disk_seq";
    case Workload::disk_rand:
        return "disk_rand";
    }
    return "?";
}

std::string_view to_string(Metric m) noexcept {
    switch (m) {
    case Metric::cpu_int_ops:
        return "cpu_int_ops";
    case Metric::cpu_fp_ops:
        return "cpu_fp_ops";
    case Metric::cpu_hash_ops:
        return "cpu_hash_ops";
    case Metric::mem_read_bw:
        return "mem_read_bw";
    case Metric::mem_write_bw:
        return "mem_write_bw";
    case Metric::mem_copy_bw:
        return "mem_copy_bw";
    case Metric::mem_latency:
        return "mem_latency";
    case Metric::disk_seq_read_bw:
        return "disk_seq_read_bw";
    case Metric::disk_seq_write_bw:
        return "disk_seq_write_bw";
    case Metric::disk_rand_read_iops:
        return "disk_rand_read_iops";
    case Metric::disk_rand_write_iops:
        return "disk_rand_write_iops";
    case Metric::disk_rand_read_p99_us:
        return "disk_rand_read_p99_us";
    }
    return "?";
}

std::string_view to_string(Unit u) noexcept {
    switch (u) {
    case Unit::ops_per_s:
        return "ops/s";
    case Unit::gb_per_s:
        return "GB/s";
    case Unit::ns:
        return "ns";
    case Unit::mb_per_s:
        return "MB/s";
    case Unit::iops:
        return "IOPS";
    case Unit::us:
        return "us";
    }
    return "?";
}

template <class E, std::size_t N>
static std::optional<E> parse_enum(const std::array<E, N>& all, std::string_view s) noexcept {
    for (E e : all)
        if (to_string(e) == s)
            return e;
    return std::nullopt;
}

std::optional<Workload> parse_workload(std::string_view s) noexcept {
    return parse_enum(kAllWorkloads, s);
}
std::optional<Metric> parse_metric(std::string_view s) noexcept {
    return parse_enum(kAllMetrics, s);
}
std::optional<Unit> parse_unit(std::string_view s) noexcept {
    return parse_enum(kAllUnits, s);
}

Unit unit_of(Metric m) noexcept {
    switch (m) {
    case Metric::cpu_int_ops:
    case Metric::cpu_fp_ops:
    case Metric::cpu_hash_ops:
        return Unit::ops_per_s;
    case Metric::mem_read_bw:
    case Metric::mem_write_bw:
    case Metric::mem_copy_bw:
        return Unit::gb_per_s;
    case Metric::mem_latency:
        return Unit::ns;
    case Metric::disk_seq_read_bw:
    case Metric::disk_seq_write_bw:
        return Unit::mb_per_s;
    case Metric::disk_rand_read_iops:
    case Metric::disk_rand_write_iops:
        return Unit::iops;
    case Metric::disk_rand_read_p99_us:
        return Unit::us;
    }
    return Unit::ops_per_s;
}

Workload workload_of(Metric m) noexcept {
    switch (m) {
    case Metric::cpu_int_ops:
        return Workload::cpu_int;
    case Metric::cpu_fp_ops:
        return Workload::cpu_fp;
    case Metric::cpu_hash_ops:
        return Workload::cpu_hash;
    case Metric::mem_read_bw:
    case Metric::mem_write_bw:
    case Metric::mem_copy_bw:
        return Workload::mem_bw;
    case Metric::mem_latency:
        return Workload::mem_latency;
    case Metric::disk_seq_read_bw:
    case Metric::disk_seq_write_bw:
        return Workload::disk_seq;
    case Metric::disk_rand_read_iops:
    case Metric::disk_rand_write_iops:
    case Metric::disk_rand_read_p99_us:
        return Workload::disk_rand;
    }
    return Workload::cpu_int;
}

bool lower_is_better(Metric m) noexcept {
    return m == Metric::mem_latency || m == Metric::disk_rand_read_p99_us;
}

} // namespace bench
