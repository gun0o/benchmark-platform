// cpu_hash: one op = hashing one 64-byte block with a 64-bit xxHash-style mixer.
#pragma once

#include "bench/cache.hpp"
#include "bench/workload.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace bench {

inline constexpr std::size_t kHashBlockBytes = 64;

// The five xxHash64 primes. They are odd 64-bit constants with well-mixed bit patterns;
// what matters for a benchmark is that they are fixed, published values rather than
// something invented here, so "xxHash-style" is a checkable claim.
inline constexpr std::uint64_t kHashP1 = 0x9E3779B185EBCA87ull;
inline constexpr std::uint64_t kHashP2 = 0xC2B2AE3D27D4EB4Full;
inline constexpr std::uint64_t kHashP3 = 0x165667B19E3779F9ull;
inline constexpr std::uint64_t kHashP4 = 0x85EBCA77C2B2AE63ull;
inline constexpr std::uint64_t kHashP5 = 0x27D4EB2F165667C5ull;

// One xxHash64 round: multiply the input into the accumulator, rotate, multiply again.
// The rotate is what stops the multiplies from only ever propagating bits upwards.
[[nodiscard]] inline std::uint64_t hash_round(std::uint64_t acc, std::uint64_t input) noexcept {
    acc += input * kHashP2;
    acc = std::rotl(acc, 31);
    return acc * kHashP1;
}

[[nodiscard]] inline std::uint64_t hash_merge(std::uint64_t acc, std::uint64_t lane) noexcept {
    acc ^= hash_round(0, lane);
    return acc * kHashP1 + kHashP4;
}

// Hash exactly 64 bytes. Structure follows xxHash64: four independent accumulator lanes
// consume the block as two 32-byte stripes, the lanes are rotated and merged into one
// value, and a final avalanche spreads every input bit over the whole output.
//
// The 64 bytes are copied into a local array and reinterpreted with std::bit_cast rather
// than pointer-cast: the buffer is bytes, the mixer wants uint64_t, and going through
// bit_cast keeps that legal regardless of the buffer's alignment. Neither the copy nor the
// bit_cast survives -O3: the disassembly (docs/results/m2.4/disasm_kernels.log) shows the
// hot loop loading the eight words with plain 64-bit movs straight into the mixer, with no
// staging array and no vector shuffle.
[[nodiscard]] inline std::uint64_t hash_block(const std::byte* block, std::uint64_t seed) noexcept {
    std::array<std::byte, kHashBlockBytes> raw{};
    std::memcpy(raw.data(), block, kHashBlockBytes);
    const auto w = std::bit_cast<std::array<std::uint64_t, 8>>(raw);

    std::uint64_t v1 = seed + kHashP1 + kHashP2;
    std::uint64_t v2 = seed + kHashP2;
    std::uint64_t v3 = seed;
    std::uint64_t v4 = seed - kHashP1;
    v1 = hash_round(v1, w[0]);
    v2 = hash_round(v2, w[1]);
    v3 = hash_round(v3, w[2]);
    v4 = hash_round(v4, w[3]);
    v1 = hash_round(v1, w[4]);
    v2 = hash_round(v2, w[5]);
    v3 = hash_round(v3, w[6]);
    v4 = hash_round(v4, w[7]);

    std::uint64_t h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    h = hash_merge(h, v1);
    h = hash_merge(h, v2);
    h = hash_merge(h, v3);
    h = hash_merge(h, v4);
    h += kHashBlockBytes;

    h ^= h >> 33; // avalanche
    h *= kHashP2;
    h ^= h >> 29;
    h *= kHashP3;
    h ^= h >> 32;
    return h ^ kHashP5;
}

// Hashes 64-byte blocks out of a small buffer, walking the blocks in order and wrapping.
//
// The buffer is 4 KiB by default, which is the point: it is larger than the mixer's own
// state (so the loads are real loads, not a register that stays put) but far inside a
// 48 KiB L1d (so the loads always hit L1 and the metric measures the mixer rather than the
// memory system). --working-set overrides the size, which is how the L1-residency claim is
// tested rather than asserted.
class CpuHashWorkload {
public:
    static constexpr int kLanes = 4; // the mixer's four accumulators, not four ops
    static constexpr std::size_t kDefaultBufferBytes = 4096;
    static constexpr std::uint64_t kBatchOps = 1u << 16; // 64 Ki blocks = 4 MiB hashed

    void setup(const WorkloadContext& ctx);
    std::uint64_t run_batch();
    void teardown() noexcept {}

    // Unlike cpu_int/cpu_fp this workload has a real input buffer, so that is what gets
    // cooled: every trial starts having to pull the 4 KiB back into L1.
    [[nodiscard]] std::span<const std::byte> cold_region() const { return buf_.span(); }

    [[nodiscard]] std::size_t buffer_bytes() const noexcept { return buf_.size(); }
    [[nodiscard]] std::size_t blocks() const noexcept { return blocks_; }
    [[nodiscard]] std::uint64_t digest() const noexcept { return acc_; }

    // Round `bytes` to a whole number of 64-byte blocks, with 0 meaning the default.
    [[nodiscard]] static std::size_t buffer_bytes_for(std::uint64_t bytes) noexcept;

private:
    Buffer buf_;
    std::size_t blocks_ = 0;
    std::size_t next_block_ = 0;
    std::uint64_t seed_ = 0;
    std::uint64_t acc_ = 0;
};
static_assert(WorkloadImpl<CpuHashWorkload>);

} // namespace bench
