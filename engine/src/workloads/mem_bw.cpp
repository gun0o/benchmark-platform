#include "bench/workloads/mem_bw.hpp"

#include "bench/timing.hpp"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <stdexcept>

namespace bench {
namespace {

// 32-byte non-temporal stores exist from AVX onwards. The `ci` preset builds for
// x86-64-v3, which has them; a hypothetical portable build without AVX still compiles,
// and --nt there is recorded as requested-but-not-effective.
#if defined(__AVX__)
inline constexpr bool kHaveNonTemporal = true;
#else
inline constexpr bool kHaveNonTemporal = false;
#endif

// ---- read --------------------------------------------------------------------------------
//
// Eight independent accumulators, not one. With a single accumulator the loop is a chain of
// dependent adds and reports add latency; with eight the adds issue in parallel and the
// loads are the limit, which is what a bandwidth metric is asking about. GCC's SLP pass
// turns the eight into two 256-bit vpaddq accumulators - checked, not assumed:
// docs/results/m3.1/disasm_mem_bw.log.
std::uint64_t sum_words(const std::uint64_t* p, std::size_t words) noexcept {
    std::uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    std::size_t i = 0;
    for (; i + 8 <= words; i += 8) {
        s0 += p[i + 0];
        s1 += p[i + 1];
        s2 += p[i + 2];
        s3 += p[i + 3];
        s4 += p[i + 4];
        s5 += p[i + 5];
        s6 += p[i + 6];
        s7 += p[i + 7];
    }
    for (; i < words; ++i)
        s0 += p[i];
    return (s0 + s1) + (s2 + s3) + ((s4 + s5) + (s6 + s7));
}

// ---- write -------------------------------------------------------------------------------
//
// `v` comes from a runtime RNG, so the compiler cannot prove its bytes are all equal and
// cannot turn the loop into memset. That matters for the metric's definition: memset is a
// legitimate way to measure write bandwidth (PLAN.md says so), but it is a different
// primitive with its own hand-written kernel, and which one ran should not depend on
// whether the optimizer happened to spot a byte-uniform constant.
void fill_words(std::uint64_t* p, std::size_t words, std::uint64_t v) noexcept {
    for (std::size_t i = 0; i < words; ++i)
        p[i] = v;
}

#if defined(__AVX__)
// Non-temporal stores write whole 32-byte chunks straight to memory, skipping the
// read-for-ownership that a normal store needs (the line has to be fetched before it can be
// partially modified). That is why --nt roughly doubles write bandwidth in the DRAM regime.
// The sfence is required: NT stores are weakly ordered with respect to everything else.
void fill_words_nt(std::uint64_t* p, std::size_t words, std::uint64_t v) noexcept {
    const __m256i vv = _mm256_set1_epi64x(static_cast<long long>(v));
    std::size_t i = 0;
    for (; i + 4 <= words; i += 4)
        _mm256_stream_si256(reinterpret_cast<__m256i*>(p + i), vv);
    for (; i < words; ++i)
        p[i] = v;
    _mm_sfence();
}

void copy_words_nt(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= words; i += 4)
        _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + i),
                            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i)));
    for (; i < words; ++i)
        dst[i] = src[i];
    _mm_sfence();
}
#endif

} // namespace

bool MemBwWorkload::non_temporal_available() noexcept {
    return kHaveNonTemporal;
}

std::uint64_t MemBwWorkload::buffer_bytes_for(std::uint64_t working_set) noexcept {
    const std::uint64_t want = working_set == 0 ? kDefaultWorkingSet : working_set;
    const std::uint64_t lines = std::max<std::uint64_t>(1, want / kCacheLine);
    return lines * kCacheLine;
}

MemBwWorkload::Mode MemBwWorkload::mode_of(Metric m) noexcept {
    switch (m) {
    case Metric::mem_write_bw:
        return Mode::write;
    case Metric::mem_copy_bw:
        return Mode::copy;
    default:
        return Mode::read;
    }
}

void MemBwWorkload::setup(const WorkloadContext& ctx) {
    mode_ = mode_of(ctx.metric);
    bytes_ = buffer_bytes_for(ctx.working_set_bytes);
    nt_requested_ = ctx.options != nullptr && ctx.options->non_temporal;
    nt_ = nt_requested_ && kHaveNonTemporal && mode_ != Mode::read;
    const bool huge = ctx.options == nullptr || ctx.options->huge_pages;

    // Copy needs a source and a destination of working_set_bytes each. They are one
    // allocation so that cold_region() covers both and so that, when the working set is a
    // multiple of 2 MiB, the destination inherits the source's huge-page alignment.
    const std::uint64_t total = mode_ == Mode::copy ? 2 * bytes_ : bytes_;
    if (total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::runtime_error("mem_bw: working set does not fit in a size_t");
    buf_ = alloc_buffer(static_cast<std::size_t>(total),
                        AllocOptions{.huge_pages = huge, .first_touch = true});

    // Real values, written on this thread. A zero-filled buffer would still be measured
    // correctly - bandwidth does not care what the bits are - but a read checksum over
    // zeros carries no information, and a sink that provably carries none is one inlining
    // decision away from letting the loop be deleted (the cpu_hash lesson, M2.4).
    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
    std::uint64_t x = ctx.seed ^ (kGolden * static_cast<std::uint64_t>(ctx.thread_index + 1));
    auto* words = reinterpret_cast<std::uint64_t*>(buf_.data());
    const std::size_t total_words = static_cast<std::size_t>(total) / sizeof(std::uint64_t);
    for (std::size_t i = 0; i < total_words; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        words[i] = x;
    }
    fill_ = x | 1u;

    passes_ = std::max<std::uint64_t>(1, kMinBatchBytes / std::max<std::uint64_t>(bytes_, 1));
    batch_bytes_ = passes_ * bytes_;
}

std::uint64_t MemBwWorkload::run_batch() {
    auto* const base = reinterpret_cast<std::uint64_t*>(buf_.data());
    const std::size_t words = static_cast<std::size_t>(bytes_) / sizeof(std::uint64_t);
    const std::uint64_t passes = passes_;

    // The mode is hoisted out of the pass loop: one branch per batch, not one per pass.
    switch (mode_) {
    // Every pass ends with a compiler barrier. Without one the passes are identical pure
    // computations over memory nothing is proven to modify, and the compiler is entitled to
    // run one and reuse the answer - which would turn a 1024-pass batch over a 4 KiB buffer
    // into a single pass reported as 1024. The barrier emits no instructions; it only stops
    // that.
    case Mode::read:
        for (std::uint64_t p = 0; p < passes; ++p) {
            acc_ += sum_words(base, words);
            ClobberMemory();
        }
        break;
    case Mode::write:
        for (std::uint64_t p = 0; p < passes; ++p) {
#if defined(__AVX__)
            if (nt_)
                fill_words_nt(base, words, fill_ + p);
            else
#endif
                fill_words(base, words, fill_ + p);
        }
        acc_ += base[0];
        break;
    case Mode::copy:
        for (std::uint64_t p = 0; p < passes; ++p) {
#if defined(__AVX__)
            if (nt_)
                copy_words_nt(base + words, base, words);
            else
#endif
                std::memcpy(base + words, base, static_cast<std::size_t>(bytes_));
            ClobberMemory();
        }
        acc_ += base[words];
        break;
    }
    DoNotOptimize(acc_);
    // Bytes, counted from the span the pass walked. For copy that is the destination bytes
    // once, not read + written: a 10 GB/s copy moves 10 GB of payload per second even
    // though the memory controller saw 20 GB of traffic. Some tools report the doubled
    // number; CLAUDE.md pins this one.
    return batch_bytes_;
}

json MemBwWorkload::describe() const {
    const char* mode = mode_ == Mode::read ? "read" : (mode_ == Mode::write ? "write" : "copy");
    return json{{"mem_mode", mode},
                {"buffer_bytes", buf_.size()},
                {"working_set_bytes", bytes_},
                {"passes_per_batch", passes_},
                {"nt", nt_requested_},
                {"nt_effective", nt_},
                {"huge_pages_requested", buf_.huge_requested()}};
}

} // namespace bench
