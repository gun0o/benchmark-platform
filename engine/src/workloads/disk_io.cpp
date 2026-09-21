#include "bench/workloads/disk_io.hpp"

#include "bench/timing.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace bench {
namespace {

std::string errno_message(const char* what, const std::string& path, int err) {
    return std::format("{} '{}': {} ({})", what, path, std::strerror(err), err);
}

// pread/pwrite can legally return fewer bytes than asked for. For an O_DIRECT transfer of a
// whole number of blocks that is not expected, but "not expected" is not "cannot happen",
// and a short read that went unnoticed would inflate every rate by however much it dropped.
std::uint64_t full_pread(int fd, void* buf, std::uint64_t len, std::uint64_t off) {
    std::uint64_t done = 0;
    while (done < len) {
        const ssize_t n =
            ::pread(fd, static_cast<std::byte*>(buf) + done, static_cast<std::size_t>(len - done),
                    static_cast<off_t>(off + done));
        if (n < 0)
            throw std::runtime_error(
                std::format("pread at {}: {}", off + done, std::strerror(errno)));
        if (n == 0)
            throw std::runtime_error(
                std::format("pread at {}: unexpected end of file", off + done));
        done += static_cast<std::uint64_t>(n);
    }
    return done;
}

std::uint64_t full_pwrite(int fd, const void* buf, std::uint64_t len, std::uint64_t off) {
    std::uint64_t done = 0;
    while (done < len) {
        const ssize_t n =
            ::pwrite(fd, static_cast<const std::byte*>(buf) + done,
                     static_cast<std::size_t>(len - done), static_cast<off_t>(off + done));
        if (n <= 0)
            throw std::runtime_error(
                std::format("pwrite at {}: {}", off + done, std::strerror(errno)));
        done += static_cast<std::uint64_t>(n);
    }
    return done;
}

} // namespace

// ---- Fd ------------------------------------------------------------------------------------

Fd::~Fd() {
    reset();
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Fd::reset() noexcept {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

// ---- the test file ---------------------------------------------------------------------------

std::string default_disk_path() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = home != nullptr ? home : ".";
    return (base / ".cache" / "bench" / "testfile").string();
}

bool is_9p_path(const std::string& path) {
    // Both the path as written and its resolved form: a symlink into /mnt is still /mnt, and
    // a relative path that lands there is too.
    std::error_code ec;
    const auto norm = std::filesystem::weakly_canonical(std::filesystem::path{path}, ec).string();
    return path.starts_with("/mnt/") || (!ec && norm.starts_with("/mnt/"));
}

void check_disk_path(const std::string& path, bool allow_9p) {
    const std::filesystem::path p{path};
    if (!allow_9p && is_9p_path(path))
        throw std::runtime_error(std::format(
            "--disk-path '{}' is under /mnt (9p/drvfs), where O_DIRECT is ignored or fails; "
            "use a path on the ext4 root, or pass --i-know-this-is-9p to override",
            path));
    std::error_code ec;
    const auto dir = p.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec)
            throw std::runtime_error(
                std::format("cannot create directory '{}': {}", dir.string(), ec.message()));
    }
}

std::uint64_t ensure_test_file(const std::string& path, std::uint64_t bytes, std::uint64_t seed) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == bytes && !ec)
        return 0; // reuse: creating it again would cost gigabytes of writes for nothing

    Fd fd{::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644)};
    if (!fd.valid())
        throw std::runtime_error(errno_message("cannot create test file", path, errno));
    if (::fallocate(fd.get(), 0, 0, static_cast<off_t>(bytes)) != 0) {
        // Not fatal: fallocate is an optimization (contiguous extents, no surprise ENOSPC
        // halfway through). The explicit fill below is what actually puts bytes on the
        // device, and it is not optional.
        if (::ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0)
            throw std::runtime_error(errno_message("cannot size test file", path, errno));
    }
    fd.reset();

    Fd out{::open(path.c_str(), O_WRONLY | O_DIRECT)};
    if (!out.valid())
        throw std::runtime_error(errno_message("cannot open test file O_DIRECT", path, errno));

    Buffer block = alloc_buffer(static_cast<std::size_t>(kDiskSeqBlock),
                                AllocOptions{.huge_pages = false, .first_touch = true});
    auto* words = reinterpret_cast<std::uint64_t*>(block.data());
    const std::size_t nwords = static_cast<std::size_t>(kDiskSeqBlock) / sizeof(std::uint64_t);
    std::uint64_t x = seed | 1u;
    std::uint64_t written = 0;
    for (std::uint64_t off = 0; off < bytes; off += kDiskSeqBlock) {
        // Fresh pseudo-random bytes per block: a repeated block would let a compressing or
        // deduplicating layer collapse the file.
        for (std::size_t i = 0; i < nwords; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            words[i] = x;
        }
        const std::uint64_t len = std::min(kDiskSeqBlock, bytes - off);
        written += full_pwrite(out.get(), block.data(), len, off);
    }
    if (::fdatasync(out.get()) != 0)
        throw std::runtime_error(errno_message("fdatasync on test file", path, errno));
    return written;
}

bool remove_test_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

// ---- DiskWorkloadBase -------------------------------------------------------------------------

void DiskWorkloadBase::open_for(const WorkloadContext& ctx, std::uint64_t block, bool writing,
                                bool dsync) {
    const WorkloadOptions* o = ctx.options;
    path_ = (o != nullptr && !o->disk_path.empty()) ? o->disk_path : default_disk_path();
    block_ = block;
    writing_ = writing;
    dsync_ = dsync;
    thread_index_ = ctx.thread_index;
    thread_count_ = std::max(1, ctx.thread_count);
    budget_ = ctx.bytes_written;

    int flags = O_DIRECT | (writing ? O_WRONLY : O_RDONLY);
    if (writing && dsync)
        flags |= O_DSYNC;
    fd_ = Fd{::open(path_.c_str(), flags)};
    if (!fd_.valid())
        throw std::runtime_error(errno_message("cannot open test file", path_, errno));

    struct stat st{};
    if (::fstat(fd_.get(), &st) != 0)
        throw std::runtime_error(errno_message("cannot stat test file", path_, errno));
    file_bytes_ = static_cast<std::uint64_t>(st.st_size);
    if (file_bytes_ < block_)
        throw std::runtime_error(std::format("test file '{}' is {} bytes, smaller than one {} "
                                             "byte block",
                                             path_, file_bytes_, block_));

    // posix_memalign(4096) inside alloc_buffer, which is what O_DIRECT requires of the
    // buffer address. No huge pages: these are 4 KiB and 1 MiB buffers.
    buf_ = alloc_buffer(static_cast<std::size_t>(block_),
                        AllocOptions{.huge_pages = false, .first_touch = true});
}

void DiskWorkloadBase::before_trial() {
    // Ask the kernel to drop this file's page-cache pages. O_DIRECT already bypasses them,
    // so this is belt and braces - the fill in ensure_test_file, a backup or a stray `cat`
    // could have left the file cached, and dropping it makes every trial start alike.
    // Return value is deliberately ignored: fadvise is advisory and a failure here is not a
    // reason to abandon a trial. It is counted so the count can be reported.
    ::posix_fadvise(fd_.get(), 0, 0, POSIX_FADV_DONTNEED);
    ++fadvise_calls_;
}

void DiskWorkloadBase::note_written(std::uint64_t bytes) noexcept {
    if (budget_ != nullptr)
        budget_->fetch_add(bytes, std::memory_order_relaxed);
}

json DiskWorkloadBase::base_describe() const {
    return json{{"disk_path", path_},      {"file_bytes", file_bytes_},
                {"block_bytes", block_},   {"queue_depth", thread_count_},
                {"o_direct", true},        {"o_dsync", dsync_},
                {"fadvise_dontneed", true}};
}

// ---- disk_seq --------------------------------------------------------------------------------

void DiskSeqWorkload::prepare_config(const WorkloadContext& ctx) {
    const WorkloadOptions* o = ctx.options;
    const std::string path =
        (o != nullptr && !o->disk_path.empty()) ? o->disk_path : default_disk_path();
    check_disk_path(path, o != nullptr && o->allow_9p);
    ensure_test_file(path, ctx.working_set_bytes, ctx.seed);
}

void DiskSeqWorkload::setup(const WorkloadContext& ctx) {
    const bool writing = ctx.metric == Metric::disk_seq_write_bw;
    open_for(ctx, kDiskSeqBlock, writing, /*dsync=*/false);

    // One contiguous region per thread, block-aligned, so N threads are N sequential
    // streams rather than one stream N threads take turns interleaving into.
    const std::uint64_t blocks = file_bytes_ / block_;
    const std::uint64_t per =
        std::max<std::uint64_t>(1, blocks / static_cast<std::uint64_t>(thread_count_));
    const auto i = static_cast<std::uint64_t>(thread_index_);
    region_off_ = std::min(i * per, blocks > 0 ? blocks - 1 : 0) * block_;
    const std::uint64_t remaining_blocks = blocks - region_off_ / block_;
    region_len_ = std::min(per, remaining_blocks) * block_;
    pos_ = 0;
    batch_units_ = kBlocksPerBatch * block_;
}

std::uint64_t DiskSeqWorkload::run_batch() {
    std::uint64_t moved = 0;
    for (std::uint64_t b = 0; b < kBlocksPerBatch; ++b) {
        const std::uint64_t off = region_off_ + pos_;
        if (writing_)
            moved += full_pwrite(fd_.get(), buf_.data(), block_, off);
        else
            moved += full_pread(fd_.get(), buf_.data(), block_, off);
        pos_ += block_;
        if (pos_ + block_ > region_len_)
            pos_ = 0; // wrap within this thread's region
    }
    if (writing_) {
        // Inside the timed region on purpose: a write benchmark that does not wait for the
        // data to reach the device is measuring the kernel's willingness to accept it.
        if (::fdatasync(fd_.get()) != 0)
            throw std::runtime_error(errno_message("fdatasync", path_, errno));
        note_written(moved);
    }
    return moved;
}

json DiskSeqWorkload::describe() const {
    json j = base_describe();
    j["blocks_per_batch"] = kBlocksPerBatch;
    j["fdatasync_per_batch"] = writing_;
    j["region_offset"] = region_off_;
    j["region_bytes"] = region_len_;
    return j;
}

// ---- disk_rand -------------------------------------------------------------------------------

void DiskRandWorkload::prepare_config(const WorkloadContext& ctx) {
    DiskSeqWorkload::prepare_config(ctx); // same file, same checks
}

void DiskRandWorkload::setup(const WorkloadContext& ctx) {
    const bool writing = ctx.metric == Metric::disk_rand_write_iops;
    // O_DSYNC for random writes only: the metric is defined as durable 4 KiB writes, which
    // is the number a database's commit path cares about.
    open_for(ctx, kDiskRandBlock, writing, /*dsync=*/writing);
    blocks_ = file_bytes_ / block_;
    batch_units_ = kIosPerBatch;

    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
    rng_.seed(ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 1)));

    // 1 us buckets to one second, plus an overflow bucket, twice over (see the header for
    // why two). Allocated and first-touched here, never in the trial loop: 4 MB of page
    // faults inside a timed region would land in the latency distribution being measured.
    for (auto& h : hist_)
        h.assign(kLatencyHistogramSize, 0u);
    used_[0] = used_[1] = 0;
    cur_ = 0;

    if (writing) {
        auto* words = reinterpret_cast<std::uint64_t*>(buf_.data());
        const std::size_t n = static_cast<std::size_t>(block_) / sizeof(std::uint64_t);
        for (std::size_t i = 0; i < n; ++i)
            words[i] = rng_();
    }
}

void DiskRandWorkload::before_trial() {
    DiskWorkloadBase::before_trial();
    // p99 is defined per trial, so the histogram starts empty every trial. Flip to the
    // other buffer - the runner is still reading this one - and clear only the buckets its
    // previous occupant reached, a few hundred of a million.
    cur_ = 1 - cur_;
    auto& h = hist_[cur_];
    if (used_[cur_] > 0) {
        std::fill(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(used_[cur_]), 0u);
        h[kLatencyOverflowBucket] = 0;
        used_[cur_] = 0;
    }
}

void DiskRandWorkload::record(std::uint64_t ns) noexcept {
    const std::uint64_t us = ns / 1000;
    const auto bucket =
        us >= kLatencyBuckets ? kLatencyOverflowBucket : static_cast<std::uint32_t>(us);
    ++hist_[cur_][bucket];
    used_[cur_] = std::max(used_[cur_], bucket + 1);
}

std::uint64_t DiskRandWorkload::run_batch() {
    std::uint64_t written = 0;
    for (std::uint64_t i = 0; i < kIosPerBatch; ++i) {
        // Uniform over the file's 4 KiB-aligned offsets. The alignment is required by
        // O_DIRECT and is also what makes this a 4 KiB random read rather than a pair of
        // straddling ones.
        const std::uint64_t off = (rng_() % blocks_) * block_;
        const auto t0 = Clock::now();
        if (writing_)
            written += full_pwrite(fd_.get(), buf_.data(), block_, off);
        else
            full_pread(fd_.get(), buf_.data(), block_, off);
        record(ns_between(t0, Clock::now()));
    }
    if (writing_)
        note_written(written);
    return kIosPerBatch;
}

json DiskRandWorkload::describe() const {
    json j = base_describe();
    j["ios_per_batch"] = kIosPerBatch;
    j["blocks_in_file"] = blocks_;
    j["latency_bucket_us"] = 1;
    j["latency_bucket_count"] = kLatencyHistogramSize;
    return j;
}

} // namespace bench
