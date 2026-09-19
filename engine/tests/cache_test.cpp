// Functional tests for the cold-cache infrastructure: what the pieces promise structurally.
// The timing claims ("a flush actually makes the next load slow") live in
// cold_cache_test.cpp, which is labelled `perf` because it asserts ratios.
#include "bench/cache.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <gtest/gtest.h>
#include <iostream>
#include <latch>
#include <set>
#include <thread>
#include <vector>

using namespace bench;

TEST(Cache, ColdModeNamesRoundTrip) {
    for (const ColdMode m : {ColdMode::none, ColdMode::clflush, ColdMode::evict}) {
        const auto s = to_string(m);
        const auto back = parse_cold_mode(s);
        ASSERT_TRUE(back.has_value()) << s;
        EXPECT_EQ(*back, m);
    }
    EXPECT_FALSE(parse_cold_mode("CLFLUSH").has_value()); // the CLI is case-sensitive
    EXPECT_FALSE(parse_cold_mode("").has_value());
    EXPECT_FALSE(parse_cold_mode("n/a").has_value()); // an outcome, never a request
}

TEST(Cache, ColdEffectNames) {
    EXPECT_EQ(to_string(ColdEffect::none), "none");
    EXPECT_EQ(to_string(ColdEffect::clflush), "clflush");
    EXPECT_EQ(to_string(ColdEffect::evict), "evict");
    EXPECT_EQ(to_string(ColdEffect::not_applicable), "n/a"); // the schema's spelling
}

// flush_lines must survive every shape of region, because a workload's region is whatever
// its allocator happened to hand it: unaligned, shorter than a line, spanning a boundary.
TEST(Cache, FlushHandlesAnyRegionShape) {
    alignas(128) std::byte block[512];
    std::memset(block, 1, sizeof(block));

    flush_lines({});                     // empty
    flush_lines({block, 1});             // one byte
    flush_lines({block, 64});            // exactly one line
    flush_lines({block + 1, 1});         // unaligned, one byte
    flush_lines({block + 63, 2});        // straddles a line boundary
    flush_lines({block + 7, 200});       // unaligned, several lines
    flush_lines({block, sizeof(block)}); // the whole thing

    // Flushing is a cache-state operation, not a memory operation: the data is still there.
    for (const std::byte b : block)
        ASSERT_EQ(b, std::byte{1});
}

TEST(Cache, ClflushoptDetectionIsStable) {
    const bool a = has_clflushopt();
    EXPECT_EQ(a, has_clflushopt()); // cached; must not flip between calls
#if defined(__CLFLUSHOPT__)
    // Built with -march=native on a machine whose compiler enabled the instruction, so the
    // runtime check had better agree.
    EXPECT_TRUE(a);
#endif
}

TEST(Cache, EvictBufferIsTwiceTheLlc) {
    EXPECT_EQ(evict_buffer_bytes(24576), std::size_t{2} * 24576 * 1024); // 24 MiB L3 -> 48 MiB
    EXPECT_EQ(evict_buffer_bytes(8192), std::size_t{2} * 8192 * 1024);
    EXPECT_EQ(evict_buffer_bytes(0), kHugePage); // unknown L3 -> 2 MiB floor, never 0
    EXPECT_EQ(evict_buffer_bytes(-1), kHugePage);
}

TEST(Cache, EvictReadsTheWholeBuffer) {
    Buffer buf = alloc_buffer(4 << 20);
    ASSERT_EQ(buf.size(), std::size_t{4} << 20);
    // Put a known value in the last line: if evict_llc stopped early the checksum below
    // could not depend on it.
    const std::uint64_t marker = 0xDEADBEEFCAFEF00D;
    std::memcpy(buf.data() + buf.size() - 64, &marker, sizeof(marker));
    const std::uint64_t a = evict_llc(buf.span());
    const std::uint64_t b = evict_llc(buf.span());
    EXPECT_EQ(a, b) << "the same bytes must hash the same way";

    const std::uint64_t other = ~marker;
    std::memcpy(buf.data() + buf.size() - 64, &other, sizeof(other));
    EXPECT_NE(evict_llc(buf.span()), a) << "the final cache line was never read";

    EXPECT_EQ(evict_llc({}), 0u); // empty span is not a crash
}

TEST(Cache, SmallBufferIsPageAlignedAndZeroed) {
    Buffer buf = alloc_buffer(256 << 10);
    ASSERT_NE(buf.data(), nullptr);
    EXPECT_EQ(buf.size(), std::size_t{256} << 10);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.data()) % 4096, 0u);
    EXPECT_FALSE(buf.huge_requested()) << "256 KiB is below the 2 MiB huge-page threshold";
    for (std::size_t i = 0; i < buf.size(); i += 4093) // a prime stride, to sample widely
        ASSERT_EQ(buf.data()[i], std::byte{0}) << "at " << i;
}

TEST(Cache, LargeBufferIsHugePageAligned) {
    // Whether the kernel *grants* huge pages depends on
    // /sys/kernel/mm/transparent_hugepage/enabled and on free contiguous memory, so the
    // grant is reported rather than asserted: this is the same measurement the engine
    // writes into params.huge_pages, and a benchmark must never claim huge pages it did
    // not get. What is asserted is the part that is under our control: the alignment,
    // without which MADV_HUGEPAGE cannot be honoured at all.
    std::cerr << "transparent huge pages, requested vs granted:\n";
    for (const std::size_t mib :
         {std::size_t{2}, std::size_t{8}, std::size_t{48}, std::size_t{128}}) {
        Buffer buf = alloc_buffer(mib << 20);
        ASSERT_NE(buf.data(), nullptr);
        EXPECT_EQ(buf.size(), mib << 20);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.data()) % kHugePage, 0u)
            << mib
            << " MiB: mapping is not 2 MiB-aligned, so the first and last huge page "
               "cannot be formed";
        EXPECT_TRUE(buf.huge_requested());
        EXPECT_LE(buf.huge_granted_bytes(), buf.size()) << "more granted than allocated";
        std::cerr << std::format("  {:4} MiB requested -> {:4} MiB in huge pages ({:5.1f}%)\n", mib,
                                 buf.huge_granted_bytes() >> 20,
                                 100.0 * static_cast<double>(buf.huge_granted_bytes()) /
                                     static_cast<double>(buf.size()));
        for (std::size_t i = 0; i < buf.size(); i += 4093)
            ASSERT_EQ(buf.data()[i], std::byte{0}) << mib << " MiB, at " << i;
    }
}

TEST(Cache, HugePagesCanBeDeclined) {
    Buffer buf = alloc_buffer(4 << 20, AllocOptions{.huge_pages = false});
    EXPECT_FALSE(buf.huge_requested());
    EXPECT_EQ(buf.huge_granted_bytes(), 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.data()) % 4096, 0u);
}

TEST(Cache, EmptyBufferIsUsable) {
    Buffer buf = alloc_buffer(0);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.span().empty());
}

TEST(Cache, BufferMovesWithoutLeakingOrDoubleFreeing) {
    Buffer a = alloc_buffer(4 << 20);
    const std::byte* addr = a.data();
    const std::size_t size = a.size();

    Buffer b = std::move(a);
    EXPECT_EQ(b.data(), addr);
    EXPECT_EQ(b.size(), size);
    EXPECT_EQ(a.data(), nullptr); // NOLINT(bugprone-use-after-move): that is the assertion
    EXPECT_TRUE(a.empty());

    Buffer c = alloc_buffer(1 << 20); // moving onto a live buffer must free the old one
    c = std::move(b);
    EXPECT_EQ(c.data(), addr);
    EXPECT_EQ(c.size(), size);
    c.data()[0] = std::byte{7}; // still mapped
    EXPECT_EQ(c.data()[0], std::byte{7});
}

TEST(Cache, SmapsReportsNothingForAStackAddress) {
    int local = 0;
    EXPECT_EQ(smaps_anon_huge_bytes(&local), 0u) << "the stack is not backed by huge pages";
    EXPECT_EQ(smaps_anon_huge_bytes(nullptr), 0u) << "address 0 is in no mapping";
}

// The decision table from cache.hpp, which is what params.cold reports.
TEST(Cache, ColdPrepDecidesWhatIsActuallyPossible) {
    constexpr int kL3Kb = 24576; // 24 MiB
    struct Case {
        ColdMode requested;
        std::size_t region_bytes;
        int l3_kb;
        ColdEffect want;
        const char* why;
    };
    const Case cases[] = {
        {ColdMode::none, 4096, kL3Kb, ColdEffect::none, "not asked for"},
        {ColdMode::none, 0, 0, ColdEffect::none, "not asked for"},
        {ColdMode::clflush, 4096, kL3Kb, ColdEffect::clflush, "fits in L3"},
        {ColdMode::clflush, std::size_t{24} << 20, kL3Kb, ColdEffect::clflush, "exactly L3"},
        {ColdMode::clflush, std::size_t{25} << 20, kL3Kb, ColdEffect::not_applicable,
         "larger than L3: it was never cache-resident"},
        {ColdMode::clflush, 0, kL3Kb, ColdEffect::not_applicable, "nothing to flush"},
        {ColdMode::clflush, std::size_t{1} << 30, 0, ColdEffect::clflush,
         "unknown L3: no basis to declare it pointless"},
        {ColdMode::evict, 0, kL3Kb, ColdEffect::evict, "does not depend on the region"},
    };
    for (const Case& c : cases) {
        ColdPrep prep;
        prep.setup(c.requested, c.l3_kb, c.region_bytes);
        EXPECT_EQ(prep.effect(), c.want)
            << to_string(c.requested) << " over " << c.region_bytes << " B: " << c.why;
        EXPECT_EQ(prep.requested(), c.requested);
    }
}

TEST(Cache, ColdPrepReportsTheBytesItTouches) {
    ColdPrep flush;
    flush.setup(ColdMode::clflush, 24576, 4096);
    EXPECT_EQ(flush.bytes(), 4096u) << "clflush reports the region";
    EXPECT_TRUE(flush.scratch().empty()) << "clflush needs no memory of its own";

    ColdPrep evict;
    evict.setup(ColdMode::evict, 24576, 4096);
    EXPECT_EQ(evict.bytes(), evict_buffer_bytes(24576)) << "evict reports its own buffer";
    EXPECT_EQ(evict.scratch().size(), evict_buffer_bytes(24576));

    ColdPrep off;
    off.setup(ColdMode::none, 24576, 4096);
    EXPECT_EQ(off.bytes(), 0u);
    EXPECT_TRUE(off.scratch().empty());
}

TEST(Cache, ColdPrepIsIdempotentAndReusable) {
    alignas(64) std::byte region[8192];
    std::memset(region, 3, sizeof(region));
    ColdPrep prep;
    for (const ColdMode m : {ColdMode::clflush, ColdMode::evict, ColdMode::none}) {
        prep.setup(m, 24576, sizeof(region)); // re-setup must release the previous buffer
        for (int i = 0; i < 4; ++i)
            prep.prepare({region, sizeof(region)});
    }
    for (const std::byte b : region)
        ASSERT_EQ(b, std::byte{3}) << "cold preparation must never modify the region";
}

// Each worker owns its own ColdPrep and its own eviction buffer; nothing is shared.
TEST(Cache, ColdPrepIsPerThread) {
    constexpr int kThreads = 4;
    std::vector<const std::byte*> bases(kThreads, nullptr);
    // The latch keeps every buffer alive until all four exist: otherwise a thread that
    // finished early could free its mapping and the next thread could be handed the same
    // address, making distinct buffers look shared.
    std::latch allocated(kThreads);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < kThreads; ++i)
            threads.emplace_back([&bases, &allocated, i] {
                alignas(64) std::byte region[4096];
                ColdPrep prep;
                prep.setup(ColdMode::evict, 2048, sizeof(region)); // 4 MiB each
                bases[static_cast<std::size_t>(i)] = prep.scratch().data();
                allocated.arrive_and_wait();
                for (int k = 0; k < 8; ++k)
                    prep.prepare({region, sizeof(region)});
            });
    }
    std::set<const std::byte*> distinct(bases.begin(), bases.end());
    EXPECT_EQ(distinct.size(), std::size_t{kThreads}) << "buffers must not be shared";
    EXPECT_EQ(distinct.count(nullptr), 0u);
}
