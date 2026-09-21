// disk_seq and disk_rand: O_DIRECT file I/O against a pre-filled test file.
//
// O_DIRECT is the whole point. A normal read goes through the guest's page cache, so a
// benchmark that reads the same file twice measures memcpy the second time. O_DIRECT tells
// the kernel to move data between the device and the caller's buffer with nothing in
// between, which is why the buffer, the offset and the length all have to be aligned to
// the device's block size - 4096 here, which satisfies both the 512-byte logical and the
// 4096-byte physical sector size.
//
// What O_DIRECT cannot do from inside WSL2 is bypass the *Windows host's* caching of the
// VHDX file that backs this ext4 filesystem. That is stated in the results rather than
// worked around, because it cannot be controlled or even observed from the guest.
#pragma once

#include "bench/cache.hpp"
#include "bench/result.hpp" // json, for describe()
#include "bench/workload.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace bench {

// ---- the test file -----------------------------------------------------------------------

inline constexpr std::uint64_t kDiskBlockAlign = 4096;
inline constexpr std::uint64_t kDiskSeqBlock = 1ull << 20; // 1 MiB sequential I/O
inline constexpr std::uint64_t kDiskRandBlock = 4096;      // 4 KiB random I/O
inline constexpr std::uint64_t kDiskDefaultSize = 4ull << 30;

// Where --disk-path points when it is not given. Never inside the repository.
std::string default_disk_path();

// True for a path under /mnt, i.e. a Windows drive mounted into WSL through drvfs (9p or
// virtiofs). Those filesystems accept O_DIRECT and ignore it, or fail it outright, so a
// benchmark there would report page-cache numbers wearing a disk's label.
bool is_9p_path(const std::string& path);

// Throws if the path is unusable for an O_DIRECT benchmark: it is a 9p path and allow_9p
// is not set, or its directory cannot be created.
void check_disk_path(const std::string& path, bool allow_9p);

// Create `path` at exactly `bytes` if it is missing or the wrong size, and fill it with
// random data through 1 MiB O_DIRECT writes followed by fdatasync.
//
// Random, never sparse and never zeros: a sparse file's "reads" never reach the device, and
// some storage stacks (and every thin-provisioned or compressing one) short-circuit runs of
// zeros. Either would turn a disk benchmark into a benchmark of the filesystem's
// bookkeeping. Returns the bytes actually written, which is 0 when the file was reused.
std::uint64_t ensure_test_file(const std::string& path, std::uint64_t bytes, std::uint64_t seed);

// Delete the test file. Returns false when there was nothing to delete.
bool remove_test_file(const std::string& path);

// An owned file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd();
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept;

private:
    int fd_ = -1;
};

// ---- shared base -------------------------------------------------------------------------

// What the two disk workloads have in common: the file, the aligned buffer, the
// per-trial page-cache drop, and the write-budget accounting.
class DiskWorkloadBase {
public:
    [[nodiscard]] std::uint64_t batch_units() const noexcept { return batch_units_; }

    // Empty: the thing that needs cooling here is the page cache, not the CPU caches, and
    // it is cooled by O_DIRECT plus posix_fadvise in before_trial(). The runner records
    // params.cold = "n/a" rather than claiming a CPU-cache cold start it did not perform.
    [[nodiscard]] std::span<const std::byte> cold_region() const { return {}; }

    // Outside the timed region, immediately before the trial's start barrier. Belt and
    // braces alongside O_DIRECT: the file may have been read through the page cache by
    // something else (the fill in ensure_test_file, a backup, the shell).
    void before_trial();

    void teardown() noexcept { fd_.reset(); }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t file_bytes() const noexcept { return file_bytes_; }
    [[nodiscard]] std::uint64_t block_bytes() const noexcept { return block_; }
    [[nodiscard]] bool writing() const noexcept { return writing_; }

protected:
    // Opens the file with the flags this metric needs and allocates the aligned buffer.
    void open_for(const WorkloadContext& ctx, std::uint64_t block, bool writing, bool dsync);
    // Records bytes against the run-wide write budget. Called after a batch, never per I/O.
    void note_written(std::uint64_t bytes) noexcept;
    [[nodiscard]] json base_describe() const;

    Fd fd_;
    Buffer buf_;
    std::string path_;
    std::uint64_t file_bytes_ = 0;
    std::uint64_t block_ = 0;
    std::uint64_t batch_units_ = 0;
    int thread_index_ = 0;
    int thread_count_ = 1;
    bool writing_ = false;
    bool dsync_ = false;
    std::atomic<std::uint64_t>* budget_ = nullptr;
    // before_trial() timing lives in params.pre_trial_prep_max_us via the runner.
    std::uint64_t fadvise_calls_ = 0;
};

// ---- disk_seq -----------------------------------------------------------------------------

class DiskSeqWorkload : public DiskWorkloadBase {
public:
    // 16 blocks of 1 MiB. For writes that is also the fdatasync interval: PLAN.md puts the
    // sync "at trial end inside the timed region", but the trial loop runs whole batches and
    // a workload cannot see a trial boundary. Syncing every 16 MiB is inside the timed
    // region and stricter than syncing once per trial, and it makes the number mean
    // "sequential write throughput including durability", which is what a disk_seq_write
    // figure is usually taken to mean anyway.
    static constexpr std::uint64_t kBlocksPerBatch = 16;

    static void prepare_config(const WorkloadContext& ctx);

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    [[nodiscard]] json describe() const;

    // For tests.
    [[nodiscard]] std::uint64_t region_offset() const noexcept { return region_off_; }
    [[nodiscard]] std::uint64_t region_bytes() const noexcept { return region_len_; }

private:
    // Each thread sweeps its own contiguous region of the file, so N threads issue N
    // sequential streams rather than N threads interleaving into one stream.
    std::uint64_t region_off_ = 0;
    std::uint64_t region_len_ = 0;
    std::uint64_t pos_ = 0; // offset within the region, wraps
};

// ---- disk_rand ----------------------------------------------------------------------------

class DiskRandWorkload : public DiskWorkloadBase {
public:
    // 32 I/Os per batch. One 4 KiB O_DIRECT read is tens to hundreds of microseconds, so a
    // batch is a few milliseconds - fine against the 500 ms trials disk uses, and short
    // enough that a trial ends close to its budget.
    static constexpr std::uint64_t kIosPerBatch = 32;

    static void prepare_config(const WorkloadContext& ctx);

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void before_trial();
    [[nodiscard]] json describe() const;

    // Per-call latencies in 1 us buckets. The runner merges these across threads after the
    // end barrier and reads disk_rand_read_p99_us off the merged histogram - the same trial
    // that produced disk_rand_read_iops, not a second run of it.
    //
    // Two histograms, alternating. The runner reads a trial's histogram *after* the end
    // barrier, and the worker is released by that same barrier straight into the next
    // trial's before_trial(). With one histogram the worker would clear the buckets while
    // the main thread was still counting them - measured, and it zeroed every p99 in the
    // run. With two, before_trial() flips and clears the buffer from two trials ago, so
    // the one being read is never touched.
    [[nodiscard]] std::span<const std::uint32_t> latency_histogram() const { return hist_[cur_]; }
    [[nodiscard]] std::uint32_t latency_used_buckets() const noexcept { return used_[cur_]; }

    // For tests.
    [[nodiscard]] std::uint64_t blocks_in_file() const noexcept { return blocks_; }

private:
    void record(std::uint64_t ns) noexcept;

    std::vector<std::uint32_t> hist_[2];
    // High-water bucket + 1 for each buffer, so a reset touches the few hundred buckets a
    // trial actually used rather than all million.
    std::uint32_t used_[2] = {0, 0};
    std::size_t cur_ = 0;
    std::uint64_t blocks_ = 0;
    std::mt19937_64 rng_;
};

} // namespace bench
