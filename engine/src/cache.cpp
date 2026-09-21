#include "bench/cache.hpp"

#include "bench/timing.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h> // _mm_clflush, _mm_sfence, _mm_mfence
#include <fstream>
#include <new>
#include <string>
#include <sys/mman.h>
#include <x86intrin.h>

namespace bench {
namespace {

// Compiled with target("clflushopt") rather than guarded by #ifdef __CLFLUSHOPT__, so the
// portable `ci` build (-march=x86-64-v3, which does not include CLFLUSHOPT) still contains
// the instruction and reaches it through the runtime CPUID check below. GCC will not inline
// a target-attributed function into a caller without that target, which is why the whole
// loop lives inside the function instead of just the instruction.
//
// The loop counts an index down instead of walking a pointer backwards past `first`: the
// latter forms an out-of-bounds pointer, which UBSan reports and the standard does not
// allow. Descending order is deliberate - see flush_lines() in the header.
[[gnu::target("clflushopt")]] void flush_range_opt(const std::byte* first,
                                                   std::size_t lines) noexcept {
    for (std::size_t i = lines; i-- > 0;)
        _mm_clflushopt(const_cast<std::byte*>(first + i * kCacheLine));
}

void flush_range_plain(const std::byte* first, std::size_t lines) noexcept {
    for (std::size_t i = lines; i-- > 0;)
        _mm_clflush(first + i * kCacheLine);
}

// One load per cache line, ascending (here we *want* the prefetcher's help filling the
// cache), with a carried dependency into DoNotOptimize so the loop cannot be elided. The
// addresses do not depend on the data, so the loads still issue in parallel: this is a
// bandwidth loop, not a latency chain.
std::uint64_t stream_read(std::span<std::byte> buf) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t off = 0; off + sizeof(std::uint64_t) <= buf.size(); off += kCacheLine) {
        std::uint64_t v;
        std::memcpy(&v, buf.data() + off, sizeof(v));
        acc = acc * 31 + v;
    }
    DoNotOptimize(acc);
    return acc;
}

std::string_view lstrip(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    return s;
}

} // namespace

std::string_view to_string(ColdMode m) noexcept {
    switch (m) {
    case ColdMode::clflush:
        return "clflush";
    case ColdMode::evict:
        return "evict";
    case ColdMode::none:
        break;
    }
    return "none";
}

std::string_view to_string(ColdEffect e) noexcept {
    switch (e) {
    case ColdEffect::clflush:
        return "clflush";
    case ColdEffect::evict:
        return "evict";
    case ColdEffect::not_applicable:
        return "n/a";
    case ColdEffect::none:
        break;
    }
    return "none";
}

std::optional<ColdMode> parse_cold_mode(std::string_view s) noexcept {
    if (s == "clflush")
        return ColdMode::clflush;
    if (s == "evict")
        return ColdMode::evict;
    if (s == "none")
        return ColdMode::none;
    return std::nullopt;
}

bool has_clflushopt() noexcept {
    // __builtin_cpu_supports consults the CPUID data gathered by the startup constructor,
    // so this is a load, not a CPUID instruction; the static keeps it to one branch anyway.
    static const bool kSupported = __builtin_cpu_supports("clflushopt") != 0;
    return kSupported;
}

void flush_lines(std::span<const std::byte> region) noexcept {
    if (region.empty())
        return;
    // Align outward: a region that starts mid-line shares that line with its neighbour, and
    // leaving it cached would leave part of the region cached.
    const auto base = reinterpret_cast<std::uintptr_t>(region.data());
    const std::uintptr_t first_addr = base & ~(kCacheLine - 1);
    const std::size_t lines = (base + region.size() - first_addr + kCacheLine - 1) / kCacheLine;
    const auto* first = reinterpret_cast<const std::byte*>(first_addr);
    if (has_clflushopt())
        flush_range_opt(first, lines);
    else
        flush_range_plain(first, lines);
    _mm_sfence(); // CLFLUSHOPT is only ordered by a fence; CLFLUSH would not need this
    _mm_mfence(); // ...and nothing after this point may be reordered ahead of the flush
}

std::size_t evict_buffer_bytes(int l3_kb) noexcept {
    const std::size_t l3 = l3_kb > 0 ? static_cast<std::size_t>(l3_kb) << 10 : 0;
    return l3 > 0 ? 2 * l3 : kHugePage;
}

std::uint64_t evict_llc(std::span<std::byte> scratch) noexcept {
    return stream_read(scratch);
}

std::string thp_policy() {
    // The file lists every policy with the active one in brackets: "always [madvise] never".
    std::ifstream in{"/sys/kernel/mm/transparent_hugepage/enabled"};
    std::string line;
    if (!in || !std::getline(in, line))
        return "unknown";
    const auto lo = line.find('[');
    const auto hi = line.find(']', lo == std::string::npos ? 0 : lo);
    if (lo == std::string::npos || hi == std::string::npos || hi <= lo + 1)
        return "unknown";
    return line.substr(lo + 1, hi - lo - 1);
}

std::uint64_t smaps_anon_huge_bytes(const void* addr) noexcept {
    const auto target = reinterpret_cast<std::uintptr_t>(addr);
    std::ifstream in{"/proc/self/smaps"};
    if (!in)
        return 0;
    std::string line;
    bool in_mapping = false;
    while (std::getline(in, line)) {
        // A mapping header is "start-end perms offset dev inode path"; a field line is
        // "Name:  value kB". Telling them apart by looking for a ':' does not work, because
        // the device field of a header contains one ("08:40"). Parsing hex up to a '-' does:
        // field names that happen to start with hex letters ("AnonHugePages") fail at the
        // first non-hex character, which is never a '-'.
        const char* b = line.data();
        const char* e = b + line.size();
        std::uintptr_t lo = 0, hi = 0;
        auto r = std::from_chars(b, e, lo, 16);
        if (r.ec == std::errc{} && r.ptr != e && *r.ptr == '-' &&
            std::from_chars(r.ptr + 1, e, hi, 16).ec == std::errc{}) {
            in_mapping = target >= lo && target < hi;
            continue;
        }
        if (in_mapping && line.starts_with("AnonHugePages:")) {
            const auto rest = lstrip(std::string_view{line}.substr(14));
            std::uint64_t kb = 0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), kb).ec == std::errc{})
                return kb << 10;
            return 0;
        }
    }
    return 0;
}

// ---- Buffer ----------------------------------------------------------------------------

Buffer::~Buffer() {
    release();
}

Buffer::Buffer(Buffer&& other) noexcept
    : data_(other.data_), size_(other.size_), mapped_(other.mapped_),
      huge_requested_(other.huge_requested_), huge_granted_(other.huge_granted_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.mapped_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        huge_requested_ = other.huge_requested_;
        huge_granted_ = other.huge_granted_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.mapped_ = 0;
    }
    return *this;
}

void Buffer::release() noexcept {
    if (data_ == nullptr)
        return;
    if (mapped_ > 0)
        ::munmap(data_, mapped_);
    else
        std::free(data_);
    data_ = nullptr;
    size_ = 0;
    mapped_ = 0;
}

Buffer alloc_buffer(std::size_t bytes, const AllocOptions& opts) {
    Buffer buf;
    if (bytes == 0)
        return buf;

    const bool want_huge = opts.huge_pages && bytes >= kHugePage;
    if (want_huge) {
        // Over-map by one huge page, cut the mapping down to a 2 MiB-aligned window, and
        // return the slack. Without the alignment the kernel cannot use huge pages for the
        // head of the mapping no matter what madvise says.
        const std::size_t span = bytes + kHugePage;
        void* raw =
            ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED)
            throw std::bad_alloc{};
        const auto start = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (start + kHugePage - 1) & ~(kHugePage - 1);
        const std::size_t head = aligned - start;
        if (head > 0)
            ::munmap(raw, head);
        const std::size_t tail = span - head - bytes;
        if (tail > 0)
            ::munmap(reinterpret_cast<void*>(aligned + bytes), tail);
        buf.data_ = reinterpret_cast<std::byte*>(aligned);
        buf.size_ = bytes;
        buf.mapped_ = bytes;
        buf.huge_requested_ = true;
        ::madvise(buf.data_, bytes, MADV_HUGEPAGE); // advisory: failure is not an error
    } else {
        void* raw = nullptr;
        if (::posix_memalign(&raw, 4096, bytes) != 0)
            throw std::bad_alloc{};
        std::memset(raw, 0, bytes); // posix_memalign does not zero; also first-touches
        buf.data_ = static_cast<std::byte*>(raw);
        buf.size_ = bytes;
    }

    if (opts.first_touch && buf.mapped_ > 0)
        for (std::size_t off = 0; off < bytes; off += 4096)
            buf.data_[off] = std::byte{0};
    // Measured for every large buffer, not only the ones that asked. Under a THP policy of
    // "always" the kernel backs a big mapping with huge pages whether or not it was advised
    // to, so params.huge_pages would otherwise read false on a run whose pages are huge -
    // which is exactly the claim a --no-hugepages comparison rests on.
    if (buf.huge_requested_ || bytes >= kHugePage)
        buf.huge_granted_ = smaps_anon_huge_bytes(buf.data_);
    return buf;
}

// ---- ColdPrep --------------------------------------------------------------------------

void ColdPrep::setup(ColdMode requested, int l3_kb, std::size_t region_bytes) {
    requested_ = requested;
    effect_ = ColdEffect::none;
    bytes_ = 0;
    scratch_ = Buffer{};

    switch (requested) {
    case ColdMode::none:
        return;
    case ColdMode::clflush: {
        const std::size_t l3 = l3_kb > 0 ? static_cast<std::size_t>(l3_kb) << 10 : 0;
        // Flushing a region that never fit in cache costs ~100 ms per 512 MiB and cools
        // nothing that was warm. Say so in the output instead of pretending.
        if (region_bytes == 0 || (l3 > 0 && region_bytes > l3)) {
            effect_ = ColdEffect::not_applicable;
            return;
        }
        effect_ = ColdEffect::clflush;
        bytes_ = region_bytes;
        return;
    }
    case ColdMode::evict:
        scratch_ = alloc_buffer(evict_buffer_bytes(l3_kb));
        effect_ = ColdEffect::evict;
        bytes_ = scratch_.size();
        return;
    }
}

void ColdPrep::prepare(std::span<const std::byte> region) noexcept {
    switch (effect_) {
    case ColdEffect::none:
    case ColdEffect::not_applicable:
        return;
    case ColdEffect::clflush:
        flush_lines(region);
        return;
    case ColdEffect::evict:
        sink_ ^= evict_llc(scratch_.span());
        DoNotOptimize(sink_);
        return;
    }
}

} // namespace bench
