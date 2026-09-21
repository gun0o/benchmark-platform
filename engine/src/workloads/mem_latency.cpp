#include "bench/workloads/mem_latency.hpp"

#include "bench/timing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace bench {
namespace {

// Words per cache line: the chase stores one "next" index per 64-byte line, in the line's
// first 8 bytes, so consecutive chase steps are always a whole line apart and two steps can
// never share a line.
inline constexpr std::uint64_t kWordsPerLine = kCacheLine / sizeof(std::uint64_t);

} // namespace

void sattolo_shuffle(std::span<std::uint64_t> a, std::uint64_t seed) noexcept {
    std::mt19937_64 rng{seed};
    for (std::size_t i = a.size(); i-- > 1;) {
        // [0, i), never [0, i]. Drawing i itself is what would allow a fixed point and, more
        // generally, a permutation with more than one cycle.
        const std::size_t j = static_cast<std::size_t>(rng() % i);
        std::swap(a[i], a[j]);
    }
}

std::uint64_t MemLatencyWorkload::buffer_bytes_for(std::uint64_t working_set) noexcept {
    const std::uint64_t want = working_set == 0 ? kDefaultWorkingSet : working_set;
    const std::uint64_t lines = std::max(kMinLines, want / kCacheLine);
    return lines * kCacheLine;
}

void MemLatencyWorkload::setup(const WorkloadContext& ctx) {
    const std::uint64_t bytes = buffer_bytes_for(ctx.working_set_bytes);
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error("mem_latency: working set does not fit in a size_t");
    const bool huge = ctx.options == nullptr || ctx.options->huge_pages;
    buf_ = alloc_buffer(static_cast<std::size_t>(bytes),
                        AllocOptions{.huge_pages = huge, .first_touch = true});
    lines_ = bytes / kCacheLine;

    // One random cyclic permutation per thread, so two threads never chase the same
    // sequence of addresses. They have separate buffers anyway, but identical orders would
    // put their accesses in lockstep, and "loaded latency" should be measured under
    // independent traffic rather than a synchronized pattern.
    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
    std::vector<std::uint64_t> order(static_cast<std::size_t>(lines_));
    for (std::uint64_t i = 0; i < lines_; ++i)
        order[static_cast<std::size_t>(i)] = i;
    sattolo_shuffle(order, ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 1)));

    // Line i's first word holds the *word* index of line order[i]'s first word, so the
    // kernel is a single load with a scaled-index address and no arithmetic of its own.
    auto* words = reinterpret_cast<std::uint64_t*>(buf_.data());
    for (std::uint64_t i = 0; i < lines_; ++i)
        words[i * kWordsPerLine] = order[static_cast<std::size_t>(i)] * kWordsPerLine;
    idx_ = 0;
}

// The whole metric is this loop. `idx` is both the result of the load and the address of
// the next one, so the loads cannot overlap: the machine has to finish one before it knows
// where the next one is. That serialization is what turns a throughput measurement into a
// latency measurement, and it is also why nothing here needs unrolling or lanes - there is
// exactly one thing in flight by construction.
//
// The address arithmetic is free: x86 addresses as base + index*8, so one iteration is one
// `mov rax,(%rdx,%rax,8)` plus the loop counter. Checked in
// docs/results/m3.2/disasm_mem_latency.log rather than assumed.
std::uint64_t MemLatencyWorkload::run_batch() {
    const std::uint64_t* const words = reinterpret_cast<const std::uint64_t*>(buf_.data());
    std::uint64_t idx = idx_;
    for (std::uint64_t i = 0; i < kBatchLoads; ++i)
        idx = words[idx];
    idx_ = idx;
    DoNotOptimize(idx_);
    return kBatchLoads;
}

std::uint64_t MemLatencyWorkload::next_line(std::uint64_t line) const noexcept {
    const auto* words = reinterpret_cast<const std::uint64_t*>(buf_.data());
    return words[line * kWordsPerLine] / kWordsPerLine;
}

json MemLatencyWorkload::describe() const {
    return json{{"chase_lines", lines_},
                {"buffer_bytes", buf_.size()},
                {"line_bytes", kCacheLine},
                {"batch_loads", kBatchLoads},
                {"huge_pages_requested", buf_.huge_requested()},
                // The chase buffer's own huge-page grant, measured from /proc/self/smaps.
                // Distinct from params.huge_pages, which describes the cold-prep scratch
                // buffer: for this workload the buffer is the whole measurement, and a
                // 64 MiB chase over 4 KiB pages is a TLB benchmark wearing a cache
                // benchmark's clothes.
                {"buffer_huge_page_bytes", buf_.huge_granted_bytes()},
                {"buffer_huge_pages", buf_.huge_granted()},
                // MADV_HUGEPAGE does nothing under "never" and is redundant under "always",
                // so the same --no-hugepages comparison means three different things under
                // the three policies. Recording it is what makes the comparison readable
                // later.
                {"thp_policy", thp_policy()}};
}

} // namespace bench
