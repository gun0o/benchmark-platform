// Machine description. Every reader takes its source as a stream or path so tests
// can feed fixture snapshots instead of the live /proc and /sys.
#pragma once

#include "bench/result.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace bench {

struct SysPaths {
    std::filesystem::path proc = "/proc";
    std::filesystem::path sys = "/sys";
    std::filesystem::path etc = "/etc";
};

struct CpuInfo {
    std::string model;
    int physical_cores = 0;
    int logical_cpus = 0;
    std::vector<std::string> flags;
};

struct CacheSizes {
    int l1d_kb = 0;
    int l2_kb = 0;
    int l3_kb = 0;
};

CpuInfo parse_cpuinfo(std::istream& in);
CacheSizes read_cache_sizes(const std::filesystem::path& cpu0_cache_dir);
std::uint64_t parse_meminfo_total_bytes(std::istream& in);
std::string parse_os_release_pretty_name(std::istream& in);
std::string detect_virtualization(std::istream& proc_version);
int parse_cache_size_kb(std::string_view text); // "48K" -> 48, "2M" -> 2048

// SHA-256 over cpu_model|physical_cores|logical_cpus|l3_kb|memory_bytes|hostname, first 32 hex.
std::string machine_id(const MachineInfo& m);

MachineInfo collect_machine_info(const SysPaths& paths = {});

} // namespace bench
