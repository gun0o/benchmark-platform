#include "bench/sysinfo.hpp"

#include "bench/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <utility>

#ifndef BENCH_VERSION
#define BENCH_VERSION "0.0.0"
#endif
#ifndef BENCH_GIT_SHA
#define BENCH_GIT_SHA "unknown"
#endif
#ifndef BENCH_COMPILER_FLAGS
#define BENCH_COMPILER_FLAGS ""
#endif

namespace bench {
namespace {

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(b, e - b + 1)};
}

// Splits "key : value" cpuinfo/meminfo lines; returns false on lines without ':'.
bool split_kv(const std::string& line, std::string& key, std::string& val) {
    const auto c = line.find(':');
    if (c == std::string::npos)
        return false;
    key = trim(std::string_view{line}.substr(0, c));
    val = trim(std::string_view{line}.substr(c + 1));
    return true;
}

std::string read_first_line(const std::filesystem::path& p) {
    std::ifstream in{p};
    std::string s;
    std::getline(in, s);
    return trim(s);
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

CpuInfo parse_cpuinfo(std::istream& in) {
    CpuInfo info;
    std::set<std::pair<int, int>> cores;
    int phys = 0, core = -1;
    std::string line, key, val;
    auto flush_block = [&] {
        if (core >= 0)
            cores.emplace(phys, core);
        phys = 0;
        core = -1;
    };
    while (std::getline(in, line)) {
        if (trim(line).empty()) {
            flush_block();
            continue;
        }
        if (!split_kv(line, key, val))
            continue;
        if (key == "processor") {
            ++info.logical_cpus;
        } else if (key == "physical id") {
            phys = std::stoi(val);
        } else if (key == "core id") {
            core = std::stoi(val);
        } else if (key == "model name" && info.model.empty()) {
            info.model = val;
        } else if (key == "flags" && info.flags.empty()) {
            std::istringstream fs{val};
            std::string f;
            while (fs >> f)
                info.flags.push_back(f);
        }
    }
    flush_block();
    info.physical_cores = cores.empty() ? info.logical_cpus : static_cast<int>(cores.size());
    return info;
}

int parse_cache_size_kb(std::string_view text) {
    const std::string t = trim(text);
    if (t.empty())
        return 0;
    std::size_t idx = 0;
    long v = 0;
    try {
        v = std::stol(t, &idx);
    } catch (...) {
        return 0;
    }
    const std::string suffix = lower(t.substr(idx));
    if (suffix == "m" || suffix == "mb")
        v *= 1024;
    else if (suffix == "g" || suffix == "gb")
        v *= 1024 * 1024;
    return static_cast<int>(v);
}

CacheSizes read_cache_sizes(const std::filesystem::path& cpu0_cache_dir) {
    CacheSizes c;
    std::error_code ec;
    if (!std::filesystem::is_directory(cpu0_cache_dir, ec))
        return c;
    for (const auto& entry : std::filesystem::directory_iterator{cpu0_cache_dir, ec}) {
        if (!entry.is_directory() || entry.path().filename().string().rfind("index", 0) != 0)
            continue;
        const int level = std::stoi("0" + read_first_line(entry.path() / "level"));
        const std::string type = lower(read_first_line(entry.path() / "type"));
        const int kb = parse_cache_size_kb(read_first_line(entry.path() / "size"));
        if (level == 1 && type == "data")
            c.l1d_kb = std::max(c.l1d_kb, kb);
        else if (level == 2)
            c.l2_kb = std::max(c.l2_kb, kb);
        else if (level == 3)
            c.l3_kb = std::max(c.l3_kb, kb);
    }
    return c;
}

std::uint64_t parse_meminfo_total_bytes(std::istream& in) {
    std::string line, key, val;
    while (std::getline(in, line)) {
        if (split_kv(line, key, val) && key == "MemTotal") {
            std::istringstream vs{val};
            std::uint64_t kb = 0;
            vs >> kb;
            return kb * 1024;
        }
    }
    return 0;
}

std::string parse_os_release_pretty_name(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return "unknown";
}

std::string detect_virtualization(std::istream& proc_version) {
    std::string line;
    std::getline(proc_version, line);
    if (lower(line).find("microsoft") != std::string::npos)
        return "wsl2";
    return "none";
}

std::string machine_id(const MachineInfo& m) {
    const std::string key = std::format("{}|{}|{}|{}|{}|{}", m.cpu_model, m.physical_cores,
                                        m.logical_cpus, m.l3_kb, m.memory_bytes, m.hostname);
    return sha256_hex(key).substr(0, 32);
}

MachineInfo collect_machine_info(const SysPaths& paths) {
    MachineInfo m;

    {
        std::ifstream in{paths.proc / "cpuinfo"};
        CpuInfo cpu = parse_cpuinfo(in);
        m.cpu_model = cpu.model.empty() ? "unknown" : cpu.model;
        m.logical_cpus = cpu.logical_cpus > 0
                             ? cpu.logical_cpus
                             : static_cast<int>(std::thread::hardware_concurrency());
        m.physical_cores = cpu.physical_cores > 0 ? cpu.physical_cores : m.logical_cpus;
    }
    {
        const CacheSizes c = read_cache_sizes(paths.sys / "devices/system/cpu/cpu0/cache");
        m.l1d_kb = c.l1d_kb;
        m.l2_kb = c.l2_kb;
        m.l3_kb = c.l3_kb;
    }
    {
        std::ifstream in{paths.proc / "meminfo"};
        m.memory_bytes = parse_meminfo_total_bytes(in);
    }
    {
        std::ifstream in{paths.etc / "os-release"};
        m.os = parse_os_release_pretty_name(in);
    }
    {
        std::ifstream in{paths.proc / "version"};
        m.virtualized = detect_virtualization(in);
    }
    {
        utsname u{};
        m.kernel = (uname(&u) == 0) ? u.release : "unknown";
    }
    {
        char host[256] = {};
        m.hostname = (gethostname(host, sizeof(host) - 1) == 0 && host[0]) ? host : "unknown";
    }
    m.compiler = std::format("g++ {}", __VERSION__);
    m.compiler_flags = BENCH_COMPILER_FLAGS;
    m.engine_version = BENCH_VERSION;
    m.engine_git_sha = BENCH_GIT_SHA;
    m.id = machine_id(m);
    return m;
}

} // namespace bench
