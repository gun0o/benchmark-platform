#include "bench/sysinfo.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace bench;
namespace fs = std::filesystem;

static const fs::path kFixture = fs::path{BENCH_FIXTURE_DIR} / "wsl2-ultra9-185h";

TEST(SysInfo, ParsesCpuinfoSnapshot) {
    std::ifstream in{kFixture / "proc/cpuinfo"};
    ASSERT_TRUE(in.good());
    const CpuInfo c = parse_cpuinfo(in);
    EXPECT_EQ(c.model, "Intel(R) Core(TM) Ultra 9 185H");
    EXPECT_EQ(c.logical_cpus, 22);
    EXPECT_EQ(c.physical_cores, 11); // unique (physical id, core id) pairs as Hyper-V presents them
    EXPECT_NE(std::find(c.flags.begin(), c.flags.end(), "clflushopt"), c.flags.end());
}

TEST(SysInfo, CpuinfoWithoutCoreIdsFallsBackToLogical) {
    std::istringstream in{"processor\t: 0\nmodel name\t: X\n\nprocessor\t: 1\nmodel name\t: X\n\n"};
    const CpuInfo c = parse_cpuinfo(in);
    EXPECT_EQ(c.logical_cpus, 2);
    EXPECT_EQ(c.physical_cores, 2);
    EXPECT_EQ(c.model, "X");
}

TEST(SysInfo, ReadsCacheSizesFromSysfsSnapshot) {
    const CacheSizes c = read_cache_sizes(kFixture / "sys/devices/system/cpu/cpu0/cache");
    EXPECT_EQ(c.l1d_kb, 48);
    EXPECT_EQ(c.l2_kb, 2048);
    EXPECT_EQ(c.l3_kb, 24576);
}

TEST(SysInfo, MissingCacheDirYieldsZeros) {
    const CacheSizes c = read_cache_sizes(kFixture / "does/not/exist");
    EXPECT_EQ(c.l1d_kb, 0);
    EXPECT_EQ(c.l2_kb, 0);
    EXPECT_EQ(c.l3_kb, 0);
}

TEST(SysInfo, ParsesCacheSizeSuffixes) {
    EXPECT_EQ(parse_cache_size_kb("48K"), 48);
    EXPECT_EQ(parse_cache_size_kb("2M"), 2048);
    EXPECT_EQ(parse_cache_size_kb("24576K"), 24576);
    EXPECT_EQ(parse_cache_size_kb(""), 0);
    EXPECT_EQ(parse_cache_size_kb("junk"), 0);
}

TEST(SysInfo, ParsesMeminfo) {
    std::ifstream in{kFixture / "proc/meminfo"};
    ASSERT_TRUE(in.good());
    EXPECT_EQ(parse_meminfo_total_bytes(in), 16173228ull * 1024);
}

TEST(SysInfo, ParsesOsReleaseAndVirtualization) {
    std::ifstream os{kFixture / "etc/os-release"};
    EXPECT_EQ(parse_os_release_pretty_name(os), "Ubuntu 22.04.5 LTS");
    std::ifstream ver{kFixture / "proc/version"};
    EXPECT_EQ(detect_virtualization(ver), "wsl2");
    std::istringstream bare{"Linux version 6.8.0-45-generic (buildd@lcy02) ..."};
    EXPECT_EQ(detect_virtualization(bare), "none");
}

TEST(SysInfo, CollectsFromFixtureRoot) {
    const MachineInfo m = collect_machine_info(
        SysPaths{.proc = kFixture / "proc", .sys = kFixture / "sys", .etc = kFixture / "etc"});
    EXPECT_EQ(m.cpu_model, "Intel(R) Core(TM) Ultra 9 185H");
    EXPECT_EQ(m.logical_cpus, 22);
    EXPECT_EQ(m.physical_cores, 11);
    EXPECT_EQ(m.l3_kb, 24576);
    EXPECT_EQ(m.memory_bytes, 16173228ull * 1024);
    EXPECT_EQ(m.os, "Ubuntu 22.04.5 LTS");
    EXPECT_EQ(m.virtualized, "wsl2");
    EXPECT_FALSE(m.kernel.empty());
    EXPECT_FALSE(m.hostname.empty());
    EXPECT_EQ(m.id.size(), 32u);
    EXPECT_EQ(m.id, machine_id(m));
    EXPECT_NE(m.compiler.find("g++"), std::string::npos);
}

TEST(SysInfo, MachineIdIsStableAndSensitiveToInputs) {
    MachineInfo a;
    a.cpu_model = "cpu";
    a.physical_cores = 4;
    a.logical_cpus = 8;
    a.l3_kb = 1024;
    a.memory_bytes = 1;
    a.hostname = "h";
    MachineInfo b = a;
    EXPECT_EQ(machine_id(a), machine_id(b));
    b.hostname = "other";
    EXPECT_NE(machine_id(a), machine_id(b));
    for (char c : machine_id(a))
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(c));
}
