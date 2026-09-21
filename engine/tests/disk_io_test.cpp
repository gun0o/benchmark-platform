// M4.1: O_DIRECT file I/O.
//
// These tests use a small real file on a real filesystem, because every interesting thing
// here is an interaction with the kernel: whether O_DIRECT's alignment rules are met,
// whether a short read is noticed, whether a thread's region of the file is the one it
// thinks it is. A mock would test none of that. The file is a few megabytes and is deleted
// afterwards; nothing here writes gigabytes.
#include "bench/metric.hpp"
#include "bench/runner.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/disk_io.hpp"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>
#include <unistd.h>
#include <vector>

using namespace bench;

namespace {

constexpr std::uint64_t kTestFileBytes = 8ull << 20; // 8 MiB: 8 seq blocks, 2048 rand blocks

// A per-test file on the same filesystem as the build tree, so it is ext4 rather than a
// tmpfs where O_DIRECT does not apply.
class DiskFixture : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = (std::filesystem::temp_directory_path() /
                 ("bench_disk_test_" + std::to_string(::getpid()) + "_" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name()))
                    .string();
        ensure_test_file(path_, kTestFileBytes, 1234);
        // If the filesystem under the temp directory cannot do O_DIRECT, every assertion
        // below would be about that rather than about the engine. Say so and skip.
        const int fd = ::open(path_.c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0)
            GTEST_SKIP() << "O_DIRECT unavailable at " << path_ << ": " << std::strerror(errno);
        ::close(fd);
    }
    void TearDown() override { remove_test_file(path_); }

    WorkloadContext ctx(Metric m, int index = 0, int threads = 1) {
        opts_.disk_path = path_;
        return WorkloadContext{.thread_index = index,
                               .thread_count = threads,
                               .working_set_bytes = kTestFileBytes,
                               .seed = 99,
                               .metric = m,
                               .options = &opts_,
                               .bytes_written = &written_};
    }

    std::string path_;
    WorkloadOptions opts_;
    std::atomic<std::uint64_t> written_{0};
};

} // namespace

// ---- the test file -------------------------------------------------------------------------

TEST(DiskPath, RecognizesNineP) {
    EXPECT_TRUE(is_9p_path("/mnt/c/Users/x/testfile"));
    EXPECT_TRUE(is_9p_path("/mnt/d/bench"));
    EXPECT_FALSE(is_9p_path("/home/user/.cache/bench/testfile"));
    EXPECT_FALSE(is_9p_path("/tmp/testfile"));
    // Not every path containing "mnt" is a mount: only the /mnt prefix counts.
    EXPECT_FALSE(is_9p_path("/home/user/mnt/testfile"));
}

TEST(DiskPath, RefusesNineP) {
    // /mnt/c is drvfs. It accepts O_DIRECT and ignores it, so a benchmark there would report
    // page-cache numbers wearing a disk's label. Refusing is the only honest option.
    try {
        check_disk_path("/mnt/c/Users/x/testfile", false);
        FAIL() << "a 9p path must be refused";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string{e.what()}.find("9p/drvfs"), std::string::npos) << e.what();
    }
    // With the override the 9p objection is gone. The path may still be unusable for other
    // reasons (an unwritable directory), which is a different complaint and is allowed here.
    try {
        check_disk_path("/mnt/c/Users/x/testfile", true);
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string{e.what()}.find("9p/drvfs"), std::string::npos) << e.what();
    }
}

TEST(DiskPath, DefaultIsOutsideTheRepository) {
    const std::string p = default_disk_path();
    EXPECT_NE(p.find(".cache/bench/testfile"), std::string::npos) << p;
    EXPECT_FALSE(p.empty());
}

TEST(DiskFile, IsCreatedAtExactlySizeAndFilledWithNonZeroData) {
    const std::string p =
        (std::filesystem::temp_directory_path() / ("bench_file_test_" + std::to_string(::getpid())))
            .string();
    const std::uint64_t bytes = 4ull << 20;
    const std::uint64_t written = ensure_test_file(p, bytes, 7);
    EXPECT_EQ(written, bytes);
    EXPECT_EQ(std::filesystem::file_size(p), bytes);

    // Never sparse, never zeros: a sparse read never reaches the device, and storage stacks
    // short-circuit runs of zeros. Check the whole file, not just the head.
    std::vector<std::byte> buf(static_cast<std::size_t>(bytes));
    const int fd = ::open(p.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::read(fd, buf.data(), buf.size()), static_cast<ssize_t>(bytes));
    ::close(fd);
    std::size_t zero_blocks = 0;
    for (std::size_t off = 0; off + 4096 <= buf.size(); off += 4096) {
        bool all_zero = true;
        for (std::size_t i = 0; i < 4096 && all_zero; ++i)
            all_zero = buf[off + i] == std::byte{0};
        zero_blocks += all_zero ? 1 : 0;
    }
    EXPECT_EQ(zero_blocks, 0u) << zero_blocks << " all-zero 4 KiB blocks in the test file";

    // Reusing an existing file of the right size writes nothing: re-filling would cost
    // gigabytes of SSD wear every run for no benefit.
    EXPECT_EQ(ensure_test_file(p, bytes, 7), 0u);
    // A different size is a different file.
    EXPECT_EQ(ensure_test_file(p, bytes * 2, 7), bytes * 2);

    EXPECT_TRUE(remove_test_file(p));
    EXPECT_FALSE(remove_test_file(p)); // nothing left to remove
}

// ---- disk_seq ---------------------------------------------------------------------------------

TEST_F(DiskFixture, SeqRegionsAreDisjointAndCoverTheFile) {
    constexpr int kThreads = 4;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> regions;
    for (int i = 0; i < kThreads; ++i) {
        DiskSeqWorkload w;
        w.setup(ctx(Metric::disk_seq_read_bw, i, kThreads));
        regions.emplace_back(w.region_offset(), w.region_bytes());
        EXPECT_EQ(w.region_offset() % kDiskSeqBlock, 0u) << "regions must be block-aligned";
        EXPECT_GT(w.region_bytes(), 0u);
        EXPECT_LE(w.region_offset() + w.region_bytes(), kTestFileBytes);
        w.teardown();
    }
    // N threads must be N sequential streams, not N threads interleaving into one.
    for (int i = 1; i < kThreads; ++i)
        EXPECT_GE(regions[static_cast<std::size_t>(i)].first,
                  regions[static_cast<std::size_t>(i) - 1].first +
                      regions[static_cast<std::size_t>(i) - 1].second);
}

TEST_F(DiskFixture, SeqReadMovesOneMibBlocksAndReportsTheBytesItMoved) {
    DiskSeqWorkload w;
    w.setup(ctx(Metric::disk_seq_read_bw));
    EXPECT_EQ(w.block_bytes(), kDiskSeqBlock);
    EXPECT_EQ(w.file_bytes(), kTestFileBytes);
    EXPECT_EQ(w.batch_units(), DiskSeqWorkload::kBlocksPerBatch * kDiskSeqBlock);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(w.run_batch(), w.batch_units());
    EXPECT_FALSE(w.writing());
    const json d = w.describe();
    EXPECT_TRUE(d["o_direct"].get<bool>());
    EXPECT_FALSE(d["o_dsync"].get<bool>());
    EXPECT_EQ(d["block_bytes"].get<std::uint64_t>(), kDiskSeqBlock);
    w.teardown();
}

TEST_F(DiskFixture, SeqReadsWrapInsideTheThreadsOwnRegion) {
    // The file is 8 MiB and a batch is 16 MiB, so a single batch must already have wrapped.
    // If it did not, it read past the region and the bytes it reported are somebody else's.
    DiskSeqWorkload w;
    w.setup(ctx(Metric::disk_seq_read_bw));
    ASSERT_LT(w.region_bytes(), DiskSeqWorkload::kBlocksPerBatch * kDiskSeqBlock);
    EXPECT_NO_THROW(w.run_batch());
    w.teardown();
}

TEST_F(DiskFixture, SeqWriteCountsAgainstTheRunWideWriteBudget) {
    DiskSeqWorkload w;
    w.setup(ctx(Metric::disk_seq_write_bw));
    EXPECT_TRUE(w.writing());
    EXPECT_TRUE(w.describe()["fdatasync_per_batch"].get<bool>());
    EXPECT_EQ(written_.load(), 0u);
    const std::uint64_t moved = w.run_batch();
    EXPECT_EQ(written_.load(), moved) << "a write metric must report what it wrote";
    w.teardown();
}

// ---- disk_rand --------------------------------------------------------------------------------

TEST_F(DiskFixture, RandReadsFourKibBlocksAndCountsIos) {
    DiskRandWorkload w;
    w.setup(ctx(Metric::disk_rand_read_iops));
    EXPECT_EQ(w.block_bytes(), kDiskRandBlock);
    EXPECT_EQ(w.blocks_in_file(), kTestFileBytes / kDiskRandBlock);
    EXPECT_EQ(w.batch_units(), DiskRandWorkload::kIosPerBatch);
    // The unit is an I/O, not a byte: IOPS counts completed calls.
    EXPECT_EQ(work_unit_of(Metric::disk_rand_read_iops), WorkUnit::ios);
    EXPECT_EQ(w.run_batch(), DiskRandWorkload::kIosPerBatch);
    w.teardown();
}

TEST_F(DiskFixture, RandRecordsOneLatencySamplePerIo) {
    DiskRandWorkload w;
    w.setup(ctx(Metric::disk_rand_read_iops));
    w.before_trial();
    w.run_batch();
    const auto h = w.latency_histogram();
    const std::uint32_t used = w.latency_used_buckets();
    ASSERT_GT(used, 0u) << "no latencies were recorded at all";
    std::uint64_t n = 0;
    for (std::uint32_t b = 0; b < used; ++b)
        n += h[b];
    EXPECT_EQ(n, DiskRandWorkload::kIosPerBatch) << "one sample per I/O, no more and no less";
    w.teardown();
}

TEST_F(DiskFixture, ThePreviousTrialsHistogramSurvivesTheNextTrialsReset) {
    // The bug this guards against was real and silent: with one histogram, the worker was
    // released from the end barrier straight into the next trial's before_trial(), which
    // cleared the buckets while the main thread was still counting them. Every p99 in the
    // run came out 0. Two histograms, alternating, is the fix.
    DiskRandWorkload w;
    w.setup(ctx(Metric::disk_rand_read_iops));

    w.before_trial();
    w.run_batch();
    const auto first = w.latency_histogram();
    const std::uint32_t first_used = w.latency_used_buckets();
    ASSERT_GT(first_used, 0u);
    std::uint64_t first_n = 0;
    for (std::uint32_t b = 0; b < first_used; ++b)
        first_n += first[b];
    ASSERT_EQ(first_n, DiskRandWorkload::kIosPerBatch);

    // The next trial begins. The runner is still holding `first`.
    w.before_trial();
    w.run_batch();
    EXPECT_NE(w.latency_histogram().data(), first.data()) << "the buffers must alternate";
    std::uint64_t still_there = 0;
    for (std::uint32_t b = 0; b < first_used; ++b)
        still_there += first[b];
    EXPECT_EQ(still_there, first_n) << "the previous trial's histogram was overwritten";
    w.teardown();
}

TEST_F(DiskFixture, RandWriteUsesDsyncAndCountsAgainstTheBudget) {
    DiskRandWorkload w;
    w.setup(ctx(Metric::disk_rand_write_iops));
    EXPECT_TRUE(w.writing());
    EXPECT_TRUE(w.describe()["o_dsync"].get<bool>())
        << "the metric is defined as durable 4 KiB writes";
    w.run_batch();
    EXPECT_EQ(written_.load(), DiskRandWorkload::kIosPerBatch * kDiskRandBlock);
    w.teardown();
}

// ---- metric plumbing --------------------------------------------------------------------------

TEST(DiskMetrics, UnitsAndRulesMatchTheDefinitions) {
    EXPECT_EQ(unit_of(Metric::disk_seq_read_bw), Unit::mb_per_s);
    EXPECT_EQ(value_rule_of(Metric::disk_seq_read_bw), ValueRule::mega_units_per_s);
    EXPECT_EQ(work_unit_of(Metric::disk_seq_read_bw), WorkUnit::bytes);
    // 1 MB is 1e6 bytes: 2e9 bytes in a second is 2000 MB/s, not 1907.
    EXPECT_DOUBLE_EQ(value_from_units(Metric::disk_seq_read_bw, 2'000'000'000ull, 1'000'000'000ull),
                     2000.0);

    EXPECT_EQ(unit_of(Metric::disk_rand_read_iops), Unit::iops);
    EXPECT_EQ(value_rule_of(Metric::disk_rand_read_iops), ValueRule::units_per_s);
    EXPECT_DOUBLE_EQ(value_from_units(Metric::disk_rand_read_iops, 5000, 1'000'000'000ull), 5000.0);

    EXPECT_EQ(unit_of(Metric::disk_rand_read_p99_us), Unit::us);
    EXPECT_EQ(value_rule_of(Metric::disk_rand_read_p99_us), ValueRule::latency_p99_us);
    EXPECT_TRUE(lower_is_better(Metric::disk_rand_read_p99_us));
    // The p99 never comes from the unit count, whatever is passed.
    EXPECT_DOUBLE_EQ(value_from_units(Metric::disk_rand_read_p99_us, 5000, 1'000'000'000ull), 0.0);
}

TEST(DiskMetrics, TheP99RidesOnTheTrialThatProducedTheIops) {
    EXPECT_TRUE(is_rider(Metric::disk_rand_read_p99_us));
    EXPECT_EQ(riders_of(Metric::disk_rand_read_iops),
              (std::vector<Metric>{Metric::disk_rand_read_p99_us}));
    // Expanding the workload must not give the rider a configuration of its own: it would
    // run a second second's worth of I/O and report a percentile of different reads.
    const auto ms = metrics_of(Workload::disk_rand);
    EXPECT_EQ(ms, (std::vector<Metric>{Metric::disk_rand_read_iops, Metric::disk_rand_write_iops}));
}

TEST(DiskMetrics, TheWorkingSetIsTheFileSizeRoundedToWholeBlocks) {
    EXPECT_EQ(effective_working_set(Workload::disk_seq, 0), kDiskDefaultSize);
    EXPECT_EQ(effective_working_set(Workload::disk_rand, 0), kDiskDefaultSize);
    EXPECT_EQ(effective_working_set(Workload::disk_seq, (8ull << 20) + 1234), 8ull << 20);
    EXPECT_EQ(effective_working_set(Workload::disk_seq, 1024), kDiskSeqBlock) << "never zero";
}

// ---- the runner's side --------------------------------------------------------------------------

TEST_F(DiskFixture, OneTrialProducesBothTheIopsAndItsP99) {
    RunConfig c;
    c.metrics = {Metric::disk_rand_read_iops};
    c.working_sets = {kTestFileBytes};
    c.trials = 2;
    c.warmup_trials = 1;
    c.warmup_ms = 0;
    c.trial_ms = 30;
    c.spin_ms = 0;
    c.cold = ColdMode::clflush; // asked for, and correctly reported as n/a below
    c.opts.disk_path = path_;

    MachineInfo m;
    m.hostname = "test-host";
    m.cpu_model = "test-cpu";
    m.physical_cores = 4;
    m.logical_cpus = 8;
    m.l3_kb = 24576;
    m.memory_bytes = 1ull << 34;
    m.os = "test-os";
    m.kernel = "test-kernel";
    m.compiler = "g++";
    m.compiler_flags = "-O3";
    m.engine_version = "0.1.0";
    m.engine_git_sha = "test";
    m.id = machine_id(m);

    const auto run = run_benchmarks(c, m, {"bench"});
    ASSERT_EQ(run.summary.size(), 2u) << "iops and its p99, from one set of trials";
    std::set<Metric> seen;
    for (const auto& s : run.summary)
        seen.insert(s.metric);
    EXPECT_EQ(seen, (std::set<Metric>{Metric::disk_rand_read_iops, Metric::disk_rand_read_p99_us}));

    // Two metrics x two trials, and the two metrics share each trial's index: they are two
    // readings of one measurement, not two measurements.
    ASSERT_EQ(run.results.size(), 4u);
    for (const auto& r : run.results) {
        EXPECT_EQ(r.workload, Workload::disk_rand);
        EXPECT_GT(r.value, 0.0) << to_string(r.metric);
        EXPECT_EQ(r.params["unit_of_work"].get<std::string>(), "ios");
        // The CPU-cache cold mode has nothing to cool here; the page cache is handled by
        // O_DIRECT plus fadvise, which is what the engine actually did.
        EXPECT_EQ(r.params["cold"].get<std::string>(), "n/a");
        EXPECT_EQ(r.params["cold_requested"].get<std::string>(), "clflush");
        EXPECT_TRUE(r.params["fadvise_dontneed"].get<bool>());
        EXPECT_GT(r.params["latency_samples"].get<std::uint64_t>(), 0u);
    }
    EXPECT_TRUE(validate_run(to_json(run)).empty());
}
