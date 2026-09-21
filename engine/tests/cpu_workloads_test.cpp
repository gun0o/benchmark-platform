// M2.4: the three CPU kernels. What is tested here is that each kernel computes the
// operation its metric claims it computes, and that the op count it reports is the number
// of those operations it actually performed. Whether the machine is fast is measured, not
// asserted; the timing-ratio checks live in cpu_scaling_test.cpp (labelled perf).
#include "bench/runner.hpp"
#include "bench/stats.hpp"
#include "bench/sysinfo.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/cpu_fp.hpp"
#include "bench/workloads/cpu_hash.hpp"
#include "bench/workloads/cpu_int.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

using namespace bench;

// ---- registry ---------------------------------------------------------------------------

TEST(CpuWorkloads, AllThreeAreRegisteredAsImplemented) {
    std::set<Workload> ready;
    for (const auto& d : workload_registry())
        if (d.implemented)
            ready.insert(d.kind);
    EXPECT_TRUE(ready.contains(Workload::cpu_int));
    EXPECT_TRUE(ready.contains(Workload::cpu_fp));
    EXPECT_TRUE(ready.contains(Workload::cpu_hash));
    // --all must not try to run an unbuilt workload: everything the registry calls ready
    // has to have a session the runner can build. mem_bw joined in M3.1, mem_latency in M3.2.
    EXPECT_TRUE(ready.contains(Workload::mem_bw));
    EXPECT_TRUE(ready.contains(Workload::mem_latency));
    EXPECT_EQ(ready.size(), 5u);
}

// ---- op accounting ----------------------------------------------------------------------

// run_batch() must return exactly the compile-time constant the runner reports as
// params.batch_ops. If these ever diverge, every ops/s number in the project is wrong by a
// constant factor and nothing else would notice.
template <class W> static void check_batch_ops_constant() {
    W w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 7});
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(w.run_batch(), W::kBatchOps);
    w.teardown();
}

TEST(CpuWorkloads, RunBatchReturnsTheDeclaredBatchOps) {
    check_batch_ops_constant<CpuIntWorkload>();
    check_batch_ops_constant<CpuFpWorkload>();
    check_batch_ops_constant<CpuHashWorkload>();
}

TEST(CpuWorkloads, LaneCountsMatchTheMetricDefinitions) {
    // CLAUDE.md pins these: 8 independent lanes for cpu_int and cpu_fp. cpu_hash's 4 lanes
    // are the mixer's accumulators, not four ops, which is why its batch size is the block
    // count and not blocks x lanes.
    EXPECT_EQ(CpuIntWorkload::kLanes, 8);
    EXPECT_EQ(CpuFpWorkload::kLanes, 8);
    EXPECT_EQ(CpuIntWorkload::kBatchOps, CpuIntWorkload::kItersPerBatch * 8);
    EXPECT_EQ(CpuFpWorkload::kBatchOps, CpuFpWorkload::kItersPerBatch * 8);
    EXPECT_EQ(CpuHashWorkload::kBatchOps, 1u << 16);
}

// ---- cpu_fp -----------------------------------------------------------------------------

TEST(CpuFp, LanesConvergeToTheAnalyticFixedPointOfAMulBPlusC) {
    // a <- a*b + c with 0 < b < 1 has the unique fixed point c / (1 - b). Landing there is
    // strong evidence the kernel really evaluated that expression the declared number of
    // times: a mul-only or add-only kernel converges somewhere else (or not at all).
    CpuFpWorkload w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 12345});
    const std::vector<double> b(w.mul().begin(), w.mul().end());
    const std::vector<double> c(w.add().begin(), w.add().end());
    w.run_batch();
    ASSERT_EQ(w.lanes().size(), 8u);
    for (std::size_t i = 0; i < 8; ++i) {
        const double expect = CpuFpWorkload::fixed_point(b[i], c[i]);
        EXPECT_NEAR(w.lanes()[i], expect, 1e-12 * expect) << "lane " << i;
    }
}

TEST(CpuFp, LanesStayNormalAndFiniteAcrossBatches) {
    // Subnormal or infinite operands would change what an FMA costs partway through a
    // trial, so the rate would depend on trial length. Neither may ever occur.
    CpuFpWorkload w;
    w.setup(WorkloadContext{.thread_index = 3, .working_set_bytes = 0, .seed = 999});
    for (int batch = 0; batch < 4; ++batch) {
        w.run_batch();
        for (double v : w.lanes()) {
            EXPECT_TRUE(std::isfinite(v));
            EXPECT_EQ(std::fpclassify(v), FP_NORMAL) << "lane went subnormal or zero: " << v;
            EXPECT_GT(std::abs(v), 0.4);
            EXPECT_LT(std::abs(v), 4.0);
        }
    }
}

TEST(CpuFp, ParametersAreContractionsAndDifferPerThread) {
    CpuFpWorkload a, b;
    a.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 5});
    b.setup(WorkloadContext{.thread_index = 1, .working_set_bytes = 0, .seed = 5});
    for (double m : a.mul()) {
        EXPECT_GE(m, 0.5);
        EXPECT_LT(m, 0.75) << "|b| >= 1 would let the lanes run away to infinity";
    }
    bool any_different = false;
    for (std::size_t i = 0; i < 8; ++i)
        any_different |= a.mul()[i] != b.mul()[i];
    EXPECT_TRUE(any_different) << "two workers must not run identical lane parameters";
}

// ---- cpu_hash ---------------------------------------------------------------------------

static std::array<std::byte, kHashBlockBytes> block_of(unsigned char fill) {
    std::array<std::byte, kHashBlockBytes> b{};
    for (std::size_t i = 0; i < b.size(); ++i)
        b[i] = static_cast<std::byte>(fill + static_cast<unsigned char>(i));
    return b;
}

TEST(CpuHash, IsDeterministic) {
    const auto b = block_of(0x11);
    EXPECT_EQ(hash_block(b.data(), 42), hash_block(b.data(), 42));
    EXPECT_NE(hash_block(b.data(), 42), hash_block(b.data(), 43)) << "seed must matter";
}

TEST(CpuHash, EveryInputBitChangesTheOutput) {
    // The claim behind the metric is "hashing one 64-byte block". If the mixer ignored some
    // of the block, the op would be cheaper than advertised and the ops/s number would be
    // measuring something smaller than it says. Flip each of the 512 bits in turn.
    auto base = block_of(0x5A);
    const std::uint64_t h0 = hash_block(base.data(), 7);
    for (std::size_t byte = 0; byte < base.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            auto flipped = base;
            flipped[byte] = static_cast<std::byte>(static_cast<unsigned char>(flipped[byte]) ^
                                                   (1u << static_cast<unsigned>(bit)));
            EXPECT_NE(hash_block(flipped.data(), 7), h0)
                << "byte " << byte << " bit " << bit << " does not reach the output";
        }
    }
}

TEST(CpuHash, OutputIsWellSpreadOverSequentialInputs) {
    // A weak mixer would leave structure in the low bits; a broken one might return a
    // constant. Cheap sanity: 4096 sequential blocks give 4096 distinct hashes, and the
    // population count of the outputs averages near 32 of 64 bits.
    std::set<std::uint64_t> seen;
    double bits = 0;
    for (std::uint64_t i = 0; i < 4096; ++i) {
        std::array<std::byte, kHashBlockBytes> b{};
        std::memcpy(b.data(), &i, sizeof(i));
        const std::uint64_t h = hash_block(b.data(), 0);
        seen.insert(h);
        bits += static_cast<double>(std::popcount(h));
    }
    EXPECT_EQ(seen.size(), 4096u) << "collisions over 4096 sequential inputs";
    EXPECT_NEAR(bits / 4096.0, 32.0, 1.5);
}

TEST(CpuHash, BufferIsBlockSizedAndL1ResidentByDefault) {
    CpuHashWorkload w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 1});
    EXPECT_EQ(w.buffer_bytes(), 4096u);
    EXPECT_EQ(w.blocks(), 64u);
    EXPECT_EQ(w.cold_region().size(), 4096u) << "the cold region is the input buffer";
    // The pitfall this guards: a buffer bigger than L1d turns cpu_hash into a memory
    // benchmark. 4 KiB is two orders of magnitude inside the 48 KiB L1d on this part.
    EXPECT_LE(w.buffer_bytes(), 32u * 1024u);
    // ...and bigger than the mixer's own state, so the loads cannot sit in registers.
    EXPECT_GT(w.buffer_bytes(), 64u);
}

TEST(CpuHash, WorkingSetOverridesTheBufferSizeInWholeBlocks) {
    EXPECT_EQ(CpuHashWorkload::buffer_bytes_for(0), 4096u);
    EXPECT_EQ(CpuHashWorkload::buffer_bytes_for(1024), 1024u);
    EXPECT_EQ(CpuHashWorkload::buffer_bytes_for(1000), 960u) << "rounded down to whole blocks";
    EXPECT_EQ(CpuHashWorkload::buffer_bytes_for(1), 64u) << "never below one block";
    CpuHashWorkload w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 8192, .seed = 1});
    EXPECT_EQ(w.buffer_bytes(), 8192u);
    EXPECT_EQ(w.blocks(), 128u);
}

TEST(CpuHash, BufferHoldsRuntimeDataThatDiffersPerThread) {
    CpuHashWorkload a, b;
    a.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 4});
    b.setup(WorkloadContext{.thread_index = 1, .working_set_bytes = 0, .seed = 4});
    EXPECT_NE(0, std::memcmp(a.cold_region().data(), b.cold_region().data(), 4096));
    // Not all zeros: a zero-filled buffer would still hash, but it would mean the setup
    // never ran and the compiler could in principle have known the input.
    bool nonzero = false;
    for (std::byte x : a.cold_region())
        nonzero |= (x != std::byte{0});
    EXPECT_TRUE(nonzero);
}

TEST(CpuHash, ConsumesEveryBlockAndFoldsThemAll) {
    // The batch is 64 Ki ops over 64 blocks, so it walks the buffer exactly 1024 times and
    // comes back to block 0. Changing one byte of the buffer must change the digest -
    // that is what shows the loads inside the loop are real.
    CpuHashWorkload w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 21});
    w.run_batch();
    const std::uint64_t d1 = w.digest();

    CpuHashWorkload w2;
    w2.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 21});
    w2.run_batch();
    EXPECT_EQ(w2.digest(), d1) << "same seed must give the same digest";

    CpuHashWorkload w3;
    w3.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = 0, .seed = 22});
    w3.run_batch();
    EXPECT_NE(w3.digest(), d1);
}

// ---- end to end through the runner -------------------------------------------------------

TEST(CpuWorkloads, EachProducesItsOwnMetricAndValidates) {
    RunConfig c;
    c.workloads = {Workload::cpu_int, Workload::cpu_fp, Workload::cpu_hash};
    c.trials = 3;
    c.warmup_trials = 1;
    c.warmup_ms = 0;
    c.trial_ms = 5;
    c.spin_ms = 0;
    c.seed = 42;
    MachineInfo m;
    m.hostname = "test-host";
    m.cpu_model = "test-cpu";
    m.physical_cores = 4;
    m.logical_cpus = 8;
    m.l3_kb = 24576;
    m.os = "test-os";
    m.kernel = "test-kernel";
    m.compiler = "g++";
    m.compiler_flags = "-O3";
    m.engine_version = "0.1.0";
    m.engine_git_sha = "test";
    m.id = machine_id(m);

    const auto run = run_benchmarks(c, m, {"bench"});
    ASSERT_EQ(run.results.size(), 9u);
    ASSERT_EQ(run.summary.size(), 3u);
    EXPECT_TRUE(validate_run(to_json(run)).empty());

    const std::array<std::pair<Workload, Metric>, 3> expect = {{
        {Workload::cpu_int, Metric::cpu_int_ops},
        {Workload::cpu_fp, Metric::cpu_fp_ops},
        {Workload::cpu_hash, Metric::cpu_hash_ops},
    }};
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(run.summary[i].workload, expect[i].first);
        EXPECT_EQ(run.summary[i].metric, expect[i].second);
        EXPECT_GT(run.summary[i].mean, 0.0);
    }
    for (const auto& r : run.results) {
        EXPECT_EQ(unit_of(r.metric), Unit::ops_per_s);
        const auto ops = r.params["ops"].get<std::uint64_t>();
        const auto batch_ops = r.params["batch_ops"].get<std::uint64_t>();
        EXPECT_EQ(ops % batch_ops, 0u) << "ops must be a whole number of batches";
        EXPECT_NEAR(r.value, static_cast<double>(ops) / (static_cast<double>(r.duration_ns) / 1e9),
                    1e-6);
    }
}

TEST(CpuWorkloads, ColdRegionIsTheInputBufferForHashAndLaneStateOtherwise) {
    RunConfig c;
    c.workloads = {Workload::cpu_int, Workload::cpu_fp, Workload::cpu_hash};
    c.trials = 1;
    c.warmup_trials = 0;
    c.warmup_ms = 0;
    c.trial_ms = 5;
    c.spin_ms = 0;
    c.cold = ColdMode::clflush;
    const auto run = run_benchmarks(c, MachineInfo{}, {"bench"});
    ASSERT_EQ(run.results.size(), 3u);
    for (const auto& r : run.results) {
        EXPECT_EQ(r.params["cold"], "clflush") << to_string(r.workload);
        const auto bytes = r.params["cold_bytes"].get<std::uint64_t>();
        if (r.workload == Workload::cpu_hash)
            EXPECT_EQ(bytes, 4096u);
        else if (r.workload == Workload::cpu_int)
            EXPECT_EQ(bytes, sizeof(CpuIntWorkload)) << "the workload object's own lane state";
        else
            EXPECT_EQ(bytes, sizeof(CpuFpWorkload)) << "the workload object's own lane state";
    }
}
