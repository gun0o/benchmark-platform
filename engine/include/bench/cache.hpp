// Cold-cache infrastructure: putting the caches into a known state before a trial.
//
// A benchmark that reuses the same buffer every trial measures a machine whose caches are
// already full of exactly the right data. That is a real number, but it is the number for
// the *second* run of a workload, and it hides the memory system entirely. Cold mode makes
// every trial start from the same cache state, and that state is "not cached".
//
// Two ways to get there, both provided here:
//   clflush  - ask the CPU to drop specific lines (CLFLUSHOPT). Precise and cheap per line,
//              but O(bytes / 64) instructions, so it only makes sense for regions that
//              could have been cache-resident in the first place.
//   evict    - read a private buffer of 2 x L3 so the cache fills with that instead.
//              Coarse and slow, but needs no instruction support and works in VMs that
//              trap or forbid CLFLUSH.
//
// Cold preparation always happens BEFORE the trial's start barrier, so its cost is never
// inside the timed region, and every thread does it for its own buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bench {

inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kHugePage = std::size_t{2} << 20;

// What the caller asked for.
enum class ColdMode { none, clflush, evict };

// What actually happened, which is not always the same thing. Recorded in params.cold so a
// result file never claims a cold start that was not performed.
enum class ColdEffect { none, clflush, evict, not_applicable };

std::string_view to_string(ColdMode m) noexcept;
std::string_view to_string(ColdEffect e) noexcept; // "none" | "clflush" | "evict" | "n/a"
std::optional<ColdMode> parse_cold_mode(std::string_view s) noexcept;

// True when the CPU advertises CLFLUSHOPT. Queried once via CPUID (__builtin_cpu_supports),
// not from the compile-time -march, so a portable build still uses it when the host has it.
bool has_clflushopt() noexcept;

// Evict every cache line overlapping `region` from the whole cache hierarchy, then fence.
// CLFLUSHOPT is unordered with respect to other stores, so an SFENCE is required after the
// loop; an MFENCE follows so nothing the caller does next is reordered before the flush.
// Lines are flushed in *descending* address order: the L2 streaming prefetcher tracks
// ascending access patterns, and walking backwards gives it nothing to follow.
void flush_lines(std::span<const std::byte> region) noexcept;

// Fill the cache with `scratch` instead of whatever the caller cares about, by reading one
// value per cache line with a data dependency the optimizer cannot remove. `scratch` must
// be private to the calling thread. Returns the accumulated value (already sunk through
// DoNotOptimize) so the caller can ignore it safely.
std::uint64_t evict_llc(std::span<std::byte> scratch) noexcept;

// Per-thread eviction buffer size for a machine with `l3_kb` of last-level cache: 2 x L3,
// floored at 2 MiB for machines that do not report an L3.
std::size_t evict_buffer_bytes(int l3_kb) noexcept;

// AnonHugePages, in bytes, of the /proc/self/smaps mapping containing `addr`. 0 when the
// mapping has no transparent huge pages or smaps is unreadable. Per-mapping rather than
// the process-wide smaps_rollup, so concurrent allocations in other threads cannot be
// misattributed to this one.
std::uint64_t smaps_anon_huge_bytes(const void* addr) noexcept;

struct AllocOptions {
    bool huge_pages = true; // madvise(MADV_HUGEPAGE) and 2 MiB alignment for large buffers
    bool first_touch = true;
};

// An owned, page-aligned, zero-initialized buffer.
//
// Small buffers come from posix_memalign(4096). Buffers of at least 2 MiB are mmap'd at a
// 2 MiB-aligned address and madvise(MADV_HUGEPAGE)'d. Measured on this machine (THP policy
// `madvise`, 48 MiB mapping, see docs/results/m2.3/thp_alignment.log):
//
//   2 MiB-aligned + madvise   48 MiB of 48 in huge pages
//   2 MiB-aligned, no madvise  0 MiB          <- the madvise is what does the work
//   unaligned + madvise       46 MiB of 48    <- head and tail fall back to 4 KiB pages
//   unaligned, no madvise      0 MiB
//
// So madvise is essential under the `madvise` policy and the alignment buys back the one
// partial huge page at each end - worth having, but a smaller effect than it sounds.
// Whether the kernel actually granted anything is measured after first touch, never
// assumed: the policy may be `never`, and the allocation can also simply fail to find
// contiguous memory.
//
// first_touch writes one byte per 4 KiB page on the calling thread, so the pages are
// faulted in and (on NUMA machines) local to whoever will use them. Never leave that to
// the timed region: a page fault is 1-2 us and there are 16384 of them in a 64 MiB buffer.
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::span<std::byte> span() noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return {data_, size_}; }

    // Reported into params.huge_pages: requested is what we asked for, granted_bytes is what
    // /proc/self/smaps says we got.
    [[nodiscard]] bool huge_requested() const noexcept { return huge_requested_; }
    [[nodiscard]] std::uint64_t huge_granted_bytes() const noexcept { return huge_granted_; }
    [[nodiscard]] bool huge_granted() const noexcept { return huge_granted_ > 0; }

private:
    friend Buffer alloc_buffer(std::size_t, const AllocOptions&);
    void release() noexcept;

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t mapped_ = 0; // bytes passed to munmap; 0 means the buffer came from malloc
    bool huge_requested_ = false;
    std::uint64_t huge_granted_ = 0;
};

// Throws std::bad_alloc on failure. bytes == 0 returns an empty Buffer.
Buffer alloc_buffer(std::size_t bytes, const AllocOptions& opts = {});

// Per-worker cold preparation: decides what a requested ColdMode actually means for one
// region on one machine, owns whatever memory that needs, and performs it once per trial.
//
// The decision is recorded as well as taken, because "what the user asked for" and "what
// the machine could do" are different facts and a result file has to carry both:
//   requested clflush, region larger than L3   -> "n/a"  (it was never cache-resident)
//   requested clflush, empty region            -> "n/a"  (there is nothing to flush)
//   requested clflush, CLFLUSHOPT missing      -> "clflush" (falls back to CLFLUSH)
//   requested evict                            -> "evict"
//   requested none                             -> "none"
class ColdPrep {
public:
    // Called on the worker thread, before the first trial. Allocates for `evict`.
    void setup(ColdMode requested, int l3_kb, std::size_t region_bytes);

    // One trial's preparation. Must be the LAST thing a worker touches before arriving at
    // the start barrier: anything else it reads afterwards can pull flushed lines back in,
    // and a hardware prefetcher following that access can pull in neighbours too.
    void prepare(std::span<const std::byte> region) noexcept;

    [[nodiscard]] ColdMode requested() const noexcept { return requested_; }
    [[nodiscard]] ColdEffect effect() const noexcept { return effect_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] const Buffer& scratch() const noexcept { return scratch_; }

private:
    ColdMode requested_ = ColdMode::none;
    ColdEffect effect_ = ColdEffect::none;
    std::size_t bytes_ = 0; // region bytes for clflush, scratch bytes for evict
    Buffer scratch_;
    std::uint64_t sink_ = 0;
};

} // namespace bench
