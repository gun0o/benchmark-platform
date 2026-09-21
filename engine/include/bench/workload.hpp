// Workload interface (a concept) and the static registry used by `bench list`.
#pragma once

#include "bench/metric.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bench {

// Knobs that belong to a workload rather than to the trial loop. They are held by the run
// configuration and handed to every worker by pointer, so a workload reads the same struct
// the CLI filled in and nothing has to be threaded through the runner field by field.
struct WorkloadOptions {
    bool non_temporal = false; // --nt: streaming (non-temporal) stores for mem_bw write/copy
    bool huge_pages = true;    // --no-hugepages turns off MADV_HUGEPAGE for large buffers
    // Disk workloads (M4.1).
    std::string disk_path;                         // --disk-path; empty => the default below
    bool allow_writes = false;                     // --allow-writes gates every write metric
    bool allow_9p = false;                         // --i-know-this-is-9p lifts the /mnt refusal
    std::uint64_t write_budget_bytes = 1ull << 30; // --write-budget; stops the run when spent
};

// Where the test file lives when --disk-path is not given: $HOME/.cache/bench/testfile.
std::string default_disk_path();

struct WorkloadContext {
    int thread_index = 0;
    int thread_count = 1; // disk workloads split the file by it and report it as queue depth
    std::uint64_t working_set_bytes = 0;
    std::uint64_t seed = 0;
    // Which of the workload's metrics this configuration is measuring. mem_bw, disk_seq and
    // disk_rand each own several, and one session measures exactly one of them at a time -
    // interleaving reads and writes inside a trial would measure neither.
    Metric metric = Metric::cpu_int_ops;
    const WorkloadOptions* options = nullptr;
    // Run-wide budget accounting for workloads that write to disk. The workload adds what
    // it wrote; the runner stops the run at a trial boundary when the budget is spent.
    std::atomic<std::uint64_t>* bytes_written = nullptr;
};

// A workload is a value type created once per worker thread. setup() may allocate;
// run_batch() must not. run_batch() returns the number of work units it performed, where
// the unit is WorkUnit-of-the-metric: operations for cpu_*, bytes for the bandwidth
// metrics, dependent loads for mem_latency, completed I/Os for disk_rand.
//
// batch_units() is that same number, known before the batch runs and never estimated from
// the clock: run_batch() always does a whole batch. The runner reports it as
// params.batch_ops and a test asserts the two agree. It is a runtime accessor rather than
// a compile-time constant because a memory batch is a whole number of passes over a buffer
// whose size is only known after setup(); the CPU workloads still define kBatchOps and
// return it here. It cannot be derived as iters x lanes in general: for cpu_hash one op is
// one 64-byte block, which the four mixing lanes process together rather than one op each.
//
// cold_region() names the bytes whose cache state the workload wants controlled: the
// buffer for a memory workload, the input block for cpu_hash, the workload's own lane
// state for cpu_int/cpu_fp. The runner flushes or evicts exactly this before every trial
// when --cold is on, so that trial 1 and trial 500 start from the same cache state. A
// workload with genuinely nothing to cool returns an empty span and the runner records
// params.cold = "n/a" rather than claiming a cold start it did not perform.
template <class W>
concept WorkloadImpl = requires(W w, const W cw, const WorkloadContext& ctx) {
    { w.setup(ctx) } -> std::same_as<void>;
    { w.run_batch() } -> std::same_as<std::uint64_t>;
    { cw.batch_units() } -> std::same_as<std::uint64_t>;
    { cw.cold_region() } -> std::same_as<std::span<const std::byte>>;
    { w.teardown() } -> std::same_as<void>;
};

// Four further hooks are *optional*. The runner detects each with a `requires` expression
// at the call site rather than demanding it of every workload, because most workloads have
// nothing to say and an empty override in five classes is noise, not uniformity:
//
//   static void prepare_config(const WorkloadContext&)
//       Once per configuration, on the main thread, before any worker exists. The disk
//       workloads create and fill the test file here - work that must happen exactly once
//       and may take a minute.
//   void before_trial()
//       On the worker, immediately before cold preparation and therefore outside the timed
//       region. Used to drop the page cache for the file and to clear a latency histogram.
//   json describe() const
//       params entries specific to this workload, collected from worker 0 after setup and
//       merged into every result's params.
//   std::span<const std::uint32_t> latency_histogram() const
//       Per-call latencies in 1 us buckets, for metrics whose value is a percentile of
//       individual operations rather than a rate (disk_rand_read_p99_us).
//
// The histogram bound. 1 us buckets up to one second, plus one overflow bucket: a 4 KiB
// read that takes longer than a second is a stall, not a latency, and lumping those
// together loses nothing a p99 would have used.
inline constexpr std::uint32_t kLatencyBuckets = 1'000'000;
inline constexpr std::uint32_t kLatencyOverflowBucket = kLatencyBuckets;
inline constexpr std::size_t kLatencyHistogramSize = kLatencyBuckets + 1;

struct WorkloadDesc {
    Workload kind;
    std::vector<Metric> metrics;
    std::string_view description;
    bool implemented; // false until its milestone lands
};

const std::vector<WorkloadDesc>& workload_registry();

// True when the workload's milestone has landed and `bench run` can actually run it.
bool is_implemented(Workload w) noexcept;

// A "rider" metric is a second reading of another metric's trial rather than a measurement
// of its own: disk_rand_read_p99_us is the 99th percentile of exactly the reads that
// disk_rand_read_iops counted. Riders never get a configuration to themselves - the session
// that measures their host metric emits them alongside it - so expanding a workload into
// metrics leaves them out and riders_of() puts them back.
bool is_rider(Metric m) noexcept;
std::vector<Metric> riders_of(Metric m) noexcept;
std::vector<Metric> metrics_of(Workload w);

// The working set a configuration actually uses. 0 means "this workload's default" and any
// other size is rounded to something the workload can use (a whole number of cache lines,
// of 4 KiB disk blocks, ...). The runner calls this when it builds the configuration list,
// so the working_set_bytes recorded in a result is the size that was really touched rather
// than the one the user typed.
//
// The cpu_* workloads are left alone: their working_set_bytes stays whatever was asked for
// (0 for cpu_int and cpu_fp, which have no memory working set at all), because CLAUDE.md
// defines the field as 0 for a pure-CPU workload and cpu_hash's 4 KiB input block is part
// of the kernel rather than a working set the user sweeps.
std::uint64_t default_working_set(Workload w) noexcept;
std::uint64_t effective_working_set(Workload w, std::uint64_t requested) noexcept;

} // namespace bench
