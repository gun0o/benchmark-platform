#include "bench/runner.hpp"

#include "bench/cache.hpp"
#include "bench/stats.hpp"
#include "bench/sync.hpp"
#include "bench/timing.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/cpu_fp.hpp"
#include "bench/workloads/cpu_hash.hpp"
#include "bench/workloads/cpu_int.hpp"
#include "bench/workloads/mem_bw.hpp"
#include "bench/workloads/mem_latency.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace bench {

const std::vector<WorkloadDesc>& workload_registry() {
    static const std::vector<WorkloadDesc> kRegistry = {
        {Workload::cpu_int,
         {Metric::cpu_int_ops},
         "64-bit integer mul/shift/xor/add on 8 lanes",
         true},
        {Workload::cpu_fp, {Metric::cpu_fp_ops}, "double-precision FMA on 8 lanes", true},
        {Workload::cpu_hash, {Metric::cpu_hash_ops}, "xxHash-style 64-byte block hashing", true},
        {Workload::mem_bw,
         {Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw},
         "streaming read / write / copy over a per-thread buffer",
         true},
        {Workload::mem_latency,
         {Metric::mem_latency},
         "dependent-load pointer chase (Sattolo cycle)",
         true},
        {Workload::disk_seq,
         {Metric::disk_seq_read_bw, Metric::disk_seq_write_bw},
         "1 MiB O_DIRECT sequential read / write",
         false},
        {Workload::disk_rand,
         {Metric::disk_rand_read_iops, Metric::disk_rand_write_iops, Metric::disk_rand_read_p99_us},
         "4 KiB O_DIRECT random read / write, QD = threads",
         false},
    };
    return kRegistry;
}

std::vector<int> default_cpu_list(int logical_cpus) {
    std::vector<int> cpus;
    for (int c = 0; c < logical_cpus; c += 2)
        cpus.push_back(c);
    for (int c = 1; c < logical_cpus; c += 2)
        cpus.push_back(c);
    return cpus;
}

bool is_implemented(Workload w) noexcept {
    for (const auto& d : workload_registry())
        if (d.kind == w)
            return d.implemented;
    return false;
}

// Metrics that are a second reading of another metric's trial rather than a measurement of
// their own. disk_rand_read_p99_us is the 99th percentile of exactly the reads that
// disk_rand_read_iops counted, so running it separately would measure a different second's
// worth of I/O and cost twice the time for a worse answer.
bool is_rider(Metric m) noexcept {
    return m == Metric::disk_rand_read_p99_us;
}

std::vector<Metric> riders_of(Metric m) noexcept {
    if (m == Metric::disk_rand_read_iops)
        return {Metric::disk_rand_read_p99_us};
    return {};
}

// The metrics a `--workload X` shorthand expands to: everything the registry lists for it,
// minus the riders, which their host metric brings along.
std::vector<Metric> metrics_of(Workload w) {
    std::vector<Metric> out;
    for (const auto& d : workload_registry())
        if (d.kind == w)
            for (Metric m : d.metrics)
                if (!is_rider(m))
                    out.push_back(m);
    return out;
}

std::uint64_t default_working_set(Workload w) noexcept {
    switch (w) {
    case Workload::mem_bw:
        return MemBwWorkload::kDefaultWorkingSet;
    case Workload::mem_latency:
        return MemLatencyWorkload::kDefaultWorkingSet;
    default:
        return 0;
    }
}

std::uint64_t effective_working_set(Workload w, std::uint64_t requested) noexcept {
    switch (w) {
    case Workload::mem_bw:
        return MemBwWorkload::buffer_bytes_for(requested);
    case Workload::mem_latency:
        return MemLatencyWorkload::buffer_bytes_for(requested);
    default:
        // cpu_* keep whatever was asked for; see the note in workload.hpp.
        return requested;
    }
}

namespace {

std::atomic<bool> g_abort{false};

// One per worker. 128 bytes so two workers' slots never share a cache line (or an
// adjacent-line prefetch pair). Only the owning worker writes it during a trial; the main
// thread reads it after the end barrier.
struct alignas(128) Slot {
    std::uint64_t ops = 0; // work units, in whatever WorkUnit this metric counts
    std::uint64_t batches = 0;
    std::uint64_t cold_ns = 0; // cost of the previous trial's cold prep, published after release
    std::uint64_t pre_ns = 0;  // ...and of the workload's own before_trial() hook
    Clock::time_point start{};
    Clock::time_point end{};
    // Per-call latency histogram, for metrics whose value is a percentile over individual
    // operations rather than a rate. The workload owns the storage; the worker publishes a
    // view of it once, and how much of it this trial used after every timed region, so the
    // main thread merges a few hundred buckets instead of a million.
    const std::uint32_t* hist = nullptr;
    std::uint32_t hist_used = 0;
    int cpu_start = -1; // sched_getcpu() right after the start barrier releases
    int cpu_end = -1;   // sched_getcpu() right after the timed region
    int assigned_cpu = -1;
    int pin_errno = 0; // non-zero if pthread_setaffinity_np failed
};
static_assert(sizeof(Slot) == 128, "Slot must occupy exactly one 128-byte padded block");
static_assert(alignof(Slot) == 128);

// Both barriers are SpinBarriers (see sync.hpp for why not std::barrier). Their completion
// functions run in the last arriver at the instant the phase completes: that is where
// t_release and t_done are stamped, so a slow waiter can never skew them.

struct WorkerFailure {
    std::mutex mu;
    std::string message;
};

// What setup turned out to mean for this configuration, published by worker 0 for the main
// thread to put in params: every worker computes the same answers from the same inputs, but
// only a worker can compute them at all, because they depend on its own buffers.
struct WorkerReport {
    std::atomic<ColdEffect> effect{ColdEffect::none};
    std::atomic<std::size_t> bytes{0};
    std::atomic<std::uint64_t> huge_bytes{0};
    std::atomic<std::uint64_t> batch_units{0};
    std::mutex mu;
    json extra = json::object(); // the workload's own describe(), if it has one
};

// Between trials a worker either spins at the start barrier or parks here.
//
// Spinning is right when the coordinator arrives within microseconds (the default, one
// configuration at a time): the vCPU stays hot and the next trial starts immediately.
// It is wrong when --interleave is on, because then five other configurations each run a
// 50 ms trial before this one's turn comes round again, and a spinning worker would spend
// that time stealing a CPU from the configuration actually being measured.
//
// So interleaved workers park on a futex (atomic::wait), which costs nothing while asleep.
// The wake-up is slow and uneven - that is exactly why M2.1 rejected std::barrier for the
// trial start - but it happens *before* the cold prep and *before* the start barrier, so
// no measurement depends on it. The barrier still releases every worker at one instant.
struct alignas(128) Gate {
    std::atomic<std::uint64_t> generation{0};

    void open() noexcept {
        generation.fetch_add(1, std::memory_order_release);
        generation.notify_all();
    }
    // `seen` is the generation this worker has already consumed. Re-reading the value
    // after the wait is what makes a wake-up that arrives before the wait harmless.
    void wait(std::uint64_t seen) const noexcept {
        while (generation.load(std::memory_order_acquire) == seen)
            generation.wait(seen, std::memory_order_acquire);
    }
};

int pin_current_thread(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

template <WorkloadImpl W>
void worker(std::stop_token st, int index, const RunConfig& cfg, const WorkloadContext& base_ctx,
            int l3_kb, SpinBarrier& start_b, SpinBarrier& end_b, std::latch& ready, Slot& slot,
            WorkerFailure& failure, WorkerReport& report, const Gate* gate) {
    if (slot.assigned_cpu >= 0)
        slot.pin_errno = pin_current_thread(slot.assigned_cpu);

    W w;
    ColdPrep cold;
    bool ok = true;
    try {
        // Allocation and first-touch happen here, on the worker's own thread, never inside
        // the timed region.
        WorkloadContext ctx = base_ctx;
        ctx.thread_index = index;
        w.setup(ctx);
        // Cold prep is set up after the workload, because what it has to do depends on how
        // big the workload's region turned out to be. Its own buffer (evict mode) is
        // allocated and first-touched here, on this thread, never in the trial loop.
        cold.setup(cfg.cold, l3_kb, w.cold_region().size());
        if constexpr (requires { w.latency_histogram(); }) {
            const auto h = w.latency_histogram();
            slot.hist = h.data();
        }
        if (index == 0) {
            report.effect.store(cold.effect(), std::memory_order_relaxed);
            report.bytes.store(cold.bytes(), std::memory_order_relaxed);
            report.huge_bytes.store(cold.scratch().huge_granted_bytes(), std::memory_order_relaxed);
            report.batch_units.store(w.batch_units(), std::memory_order_relaxed);
            if constexpr (requires { w.describe(); }) {
                std::lock_guard lock{report.mu};
                report.extra = w.describe();
            }
        }
    } catch (const std::exception& e) {
        ok = false;
        std::lock_guard lock{failure.mu};
        if (failure.message.empty())
            failure.message = std::format("worker {}: setup failed: {}", index, e.what());
    }
    ready.count_down(); // main waits on this latch before the first trial

    // Workers do not know how many trials there will be: the main thread decides warmup vs
    // timed per trial and ends the loop by requesting stop before its final arrival.
    const auto trial_len = std::chrono::milliseconds(cfg.trial_ms);
    std::uint64_t cold_ns = 0, pre_ns = 0;
    std::uint64_t gate_seen = 0;
    for (;;) {
        // When interleaving, sleep until this configuration's turn. Parking happens before
        // the cold prep so the flush is still the last thing touched before the barrier.
        if (gate != nullptr) {
            gate->wait(gate_seen);
            ++gate_seen;
        }
        // Cold preparation, then nothing else before the barrier. Anything touched after
        // the flush can pull the flushed lines (or, via a prefetcher, their neighbours)
        // straight back in. The barrier itself only touches the barrier's own cache lines.
        if (ok) {
            // The workload's own pre-trial work comes first and is timed separately: for a
            // disk workload it is a posix_fadvise(DONTNEED) over gigabytes, which is cold
            // preparation of a different cache and has nothing to do with CLFLUSH's cost.
            const auto p0 = Clock::now();
            if constexpr (requires { w.before_trial(); })
                w.before_trial();
            const auto c0 = Clock::now();
            cold.prepare(w.cold_region());
            const auto c1 = Clock::now();
            pre_ns = ns_between(p0, c0);
            cold_ns = ns_between(c0, c1);
        }
        start_b.arrive_and_wait();
        if (st.stop_requested())
            break; // main requested stop before arriving: consistent view

        // Every write to `slot` happens between the start and end barriers, so the main
        // thread's reads after the end barrier never race with the next trial's writes.
        // cold_ns is measured before the barrier but published here for that reason.
        slot.cold_ns = cold_ns;
        slot.pre_ns = pre_ns;
        slot.cpu_start = sched_getcpu();
        std::uint64_t ops = 0, batches = 0;
        const auto t0 = Clock::now();
        auto now = t0;
        if (ok) {
            do {
                ops += w.run_batch();
                ++batches;
                now = Clock::now();
            } while (now - t0 < trial_len);
        }
        slot.start = t0;
        slot.end = now;
        slot.ops = ops;
        slot.batches = batches;
        if constexpr (requires { w.latency_used_buckets(); })
            slot.hist_used = w.latency_used_buckets();
        slot.cpu_end = sched_getcpu();
        end_b.arrive_and_wait();
    }
    if (ok)
        w.teardown();
}

// Why a configuration stopped early, if it did.
enum class StopReason { none, aborted, timed_out, write_budget };

// Merged per-call latencies for one trial. Only the metrics whose value is a percentile of
// individual operations use this; everything else leaves it at zero.
struct LatencySummary {
    double p99_us = 0;
    std::uint64_t samples = 0;
    std::uint64_t overflow = 0; // calls slower than the histogram's one-second bound
    double min_us = 0;
    double max_us = 0;
};

// p99 over 1 us buckets, nearest rank: the smallest bucket whose cumulative count reaches
// ceil(0.99 * n), reported as that bucket's upper edge.
//
// This is deliberately not the numpy-"linear" percentile that stats.hpp uses for trial
// values. Interpolating between two adjacent 1 us buckets would invent a precision the
// histogram does not have: the underlying samples were already rounded to a microsecond on
// the way in. Nearest rank on the upper edge gives "99 % of reads completed within X us",
// which is the sentence a p99 is meant to support, and it is quantized by at most 1 us -
// well under 1 % of a 4 KiB read on any storage this would be pointed at.
LatencySummary merge_latencies(const std::vector<Slot>& slots) {
    LatencySummary out;
    std::uint32_t used = 0;
    for (const Slot& s : slots)
        if (s.hist != nullptr)
            used = std::max(used, s.hist_used);
    if (used == 0)
        return out;

    for (std::uint32_t b = 0; b < used; ++b)
        for (const Slot& s : slots)
            if (s.hist != nullptr)
                out.samples += s.hist[b];
    if (out.samples == 0)
        return out;
    if (used > kLatencyBuckets)
        for (const Slot& s : slots)
            if (s.hist != nullptr)
                out.overflow += s.hist[kLatencyOverflowBucket];

    const auto rank =
        static_cast<std::uint64_t>(std::ceil(0.99 * static_cast<double>(out.samples) - 1e-9));
    std::uint64_t cum = 0;
    bool have_min = false;
    for (std::uint32_t b = 0; b < used; ++b) {
        std::uint64_t here = 0;
        for (const Slot& s : slots)
            if (s.hist != nullptr)
                here += s.hist[b];
        if (here == 0)
            continue;
        if (!have_min) {
            out.min_us = static_cast<double>(b) + 1.0;
            have_min = true;
        }
        out.max_us = static_cast<double>(b) + 1.0;
        cum += here;
        if (out.p99_us == 0 && cum >= rank)
            out.p99_us = static_cast<double>(b) + 1.0;
    }
    return out;
}

// A trial whose workers did not all start within this much of the barrier release had a
// late worker. That is host preemption, not the workload (M2.5 pitfall): such trials are
// counted and reported next to the CoV, never dropped.
inline constexpr double kLateTrialUs = 1000.0;

// The per-configuration clock canary's detection window.
//
// The canary can only see contention that happens *while it is sampling*, so what matters
// is how long it watches for. Measured against 66 spinners on 22 vCPUs
// (docs/results/m2.5/canary_sensitivity.log):
//
//     samples   window    quiet    loaded       verdict under load
//      10,000   0.19 ms   18.6 ns    27.0 ns    MISSED - window fits between preemptions
//     100,000   1.41 ms   14.1 ns   147.3 ns    caught, but a 1.4 ms window is easy to miss
//     500,000   8.41 ms   16.8 ns   176.0 ns    caught
//   2,000,000  30.41 ms   15.2 ns   179.6 ns    caught
//
// The window is expressed as a time, not a sample count, because a count is not a window:
// the same 500,000 samples take 8 ms in a release build and most of a second under ASan,
// where they were measured eating a whole --max-seconds budget before a single trial ran.
// The sample number below is only an upper bound for a pathologically cheap clock.
//
// The failure is one-sided: a contended host can read clean, a quiet one never reads
// dirty. So the rule can keep bad data, but it can never discard good data.
inline constexpr auto kCanaryWindow = std::chrono::milliseconds{8};
inline constexpr int kCanaryMaxSamples = 2'000'000;
// A quiet host gives mean/min within a few per cent of 1 in every build measured (release
// ~1.05, ASan ~1.05). Contention pushes it to 10 or more. 3 is a wide margin either way.
inline constexpr double kCanaryMaxRatio = 3.0;

// One configuration's live worker pool, as an object the coordinator can step one trial at
// a time. Splitting the old run_config() loop this way is what makes --interleave possible:
// several sessions are alive at once and the coordinator rotates between them, instead of
// each configuration owning the machine until it finishes.
//
// Sessions are pinned in memory (the workers hold references to the barriers and slots), so
// they are heap-allocated and neither copyable nor movable.
class ISession {
public:
    virtual ~ISession() = default;
    virtual void prepare() = 0; // once-per-configuration main-thread work, before start()
    virtual void start() = 0;   // spawn the pool; throws on setup failure
    virtual bool wants_more_trials() const = 0;
    virtual void run_one_trial(const ProgressFn&) = 0;
    virtual void stop() = 0; // stop and join the workers
    virtual void emit(RunEnvelope& out, const ClockCheck& canary_start,
                      const ClockCheck& canary_end, int attempts) = 0;
    virtual int timed_done() const = 0;
};

template <WorkloadImpl W> class ConfigSession final : public ISession {
public:
    // metrics[0] is what the trial loop measures, and its value follows from the work units
    // the workers counted. Any further metrics are read off the same trial by another route
    // - today that is disk_rand_read_p99_us, a percentile of the individual reads that
    // produced disk_rand_read_iops. They are one trial with two answers, not two trials.
    ConfigSession(const RunConfig& cfg, Workload kind, std::vector<Metric> metrics, int n_threads,
                  std::uint64_t working_set, int l3_kb, const ClockCheck& clock,
                  const std::vector<int>& cpu_list, bool park,
                  std::atomic<std::uint64_t>* bytes_written)
        : cfg_(cfg), kind_(kind), metrics_(std::move(metrics)), n_threads_(n_threads),
          working_set_(working_set), l3_kb_(l3_kb), clock_(clock), park_(park),
          start_b_(n_threads + 1, [this] { t_release_ = Clock::now(); }),
          end_b_(n_threads + 1, [this] { t_done_ = Clock::now(); }), ready_(n_threads),
          slots_(static_cast<std::size_t>(n_threads)) {
        for (int i = 0; i < n_threads; ++i) {
            const int cpu = cfg.pin ? cpu_list[static_cast<std::size_t>(i) % cpu_list.size()] : -1;
            slots_[static_cast<std::size_t>(i)].assigned_cpu = cpu;
            if (cfg.pin)
                assigned_.push_back(cpu);
        }
        ctx_.thread_count = n_threads;
        ctx_.working_set_bytes = working_set;
        ctx_.seed = cfg.seed;
        ctx_.metric = metrics_.front();
        ctx_.options = &cfg.opts;
        ctx_.bytes_written = bytes_written;
        values_.resize(metrics_.size());
        for (auto& v : values_)
            v.reserve(static_cast<std::size_t>(cfg.trials));
        results_.reserve(static_cast<std::size_t>(cfg.trials) * metrics_.size());
    }

    // Once per configuration, on the main thread, before any worker exists: creating and
    // filling a multi-gigabyte test file is not something 16 workers should each attempt.
    void prepare() override {
        if constexpr (requires { W::prepare_config(ctx_); })
            W::prepare_config(ctx_);
    }
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;
    ~ConfigSession() override { stop(); }

    void start() override {
        threads_.reserve(static_cast<std::size_t>(n_threads_));
        for (int i = 0; i < n_threads_; ++i)
            threads_.emplace_back(worker<W>, i, std::cref(cfg_), std::cref(ctx_), l3_kb_,
                                  std::ref(start_b_), std::ref(end_b_), std::ref(ready_),
                                  std::ref(slots_[static_cast<std::size_t>(i)]), std::ref(failure_),
                                  std::ref(report_), park_ ? &gate_ : nullptr);
        // Fail fast if any worker could not set up. Workers that failed still take part in
        // the barriers (with zero ops) so the ones that succeeded are not left waiting
        // forever; we stop them at the first trial boundary.
        ready_.wait();
        std::lock_guard lock{failure_.mu};
        if (!failure_.message.empty()) {
            stop();
            throw std::runtime_error(failure_.message);
        }
    }

    // Warmup: at least warmup_trials trials AND at least warmup_ms of time spent running
    // *this* configuration. The time floor matters on VMs, where the first ~250 ms of a
    // configuration show multi-ms start spreads while the hypervisor settles the now-busy
    // vCPUs onto physical cores. It counts trial time rather than wall-clock time since the
    // configuration started, because under --interleave most of the wall clock is other
    // configurations' trials and would satisfy the floor without warming anything.
    [[nodiscard]] bool in_warmup() const {
        return warmups_done_ < cfg_.warmup_trials ||
               active_ns_ < static_cast<std::uint64_t>(cfg_.warmup_ms) * 1'000'000u;
    }
    [[nodiscard]] bool wants_more_trials() const override {
        return in_warmup() || timed_done_ < cfg_.trials;
    }
    [[nodiscard]] int timed_done() const override { return timed_done_; }

    void stop() override {
        if (threads_.empty())
            return;
        for (auto& th : threads_)
            th.request_stop();
        if (park_)
            gate_.open(); // wake parked workers so they can reach the stop check
        start_b_.arrive_and_wait();
        threads_.clear(); // jthread destructors join
    }

    void run_one_trial(const ProgressFn& progress) override {
        const bool warmup = in_warmup();
        if (park_)
            gate_.open(); // must precede our own arrival, or the parked workers never come
        start_b_.arrive_and_wait(); // main is usually last here: it runs the t_release stamp
        constexpr auto kMainPoll = std::chrono::microseconds{50};
        end_b_.arrive_and_wait_polling(kMainPoll); // last *worker* arrives last, stamps t_done
        if (warmup)
            ++warmups_done_;
        else
            ++timed_done_;

        std::uint64_t total_ops = 0, cold_ns_sum = 0, cold_ns_max = 0, pre_ns_max = 0;
        auto first_start = slots_[0].start, last_start = slots_[0].start;
        json per_thread_ops = json::array(), per_thread_cpu = json::array(),
             per_thread_start_us = json::array();
        int pin_violations = 0, pin_failures = 0;
        for (const Slot& s : slots_) {
            total_ops += s.ops;
            cold_ns_sum += s.cold_ns;
            cold_ns_max = std::max(cold_ns_max, s.cold_ns);
            pre_ns_max = std::max(pre_ns_max, s.pre_ns);
            first_start = std::min(first_start, s.start);
            last_start = std::max(last_start, s.start);
            const auto own_ns = ns_between(s.start, s.end);
            per_thread_ops.push_back(
                own_ns ? static_cast<double>(s.ops) / (static_cast<double>(own_ns) / 1e9) : 0.0);
            per_thread_cpu.push_back(s.cpu_end);
            per_thread_start_us.push_back(static_cast<double>(ns_between(t_release_, s.start)) /
                                          1e3);
            if (s.assigned_cpu >= 0) {
                if (s.pin_errno != 0)
                    ++pin_failures;
                else if (s.cpu_start != s.assigned_cpu || s.cpu_end != s.assigned_cpu)
                    ++pin_violations;
            }
        }
        const std::uint64_t ns = ns_between(t_release_, t_done_);
        // Time actually spent running this configuration, which is what the warmup floor
        // counts. Under --interleave the wall clock is mostly other configurations.
        active_ns_ += ns;
        const double spread_us = static_cast<double>(ns_between(first_start, last_start)) / 1e3;
        // Reading the histograms happens here, between the end barrier and the next trial's
        // start barrier, so it is outside every timed region and cannot race a worker.
        const LatencySummary lat = merge_latencies(slots_);

        const auto effect = report_.effect.load(std::memory_order_relaxed);
        json params = json{// What actually happened, then what was asked for: they differ when the
                           // region could not have been cache-resident in the first place ("n/a").
                           {"cold", to_string(effect)},
                           {"cold_requested", to_string(cfg_.cold)},
                           {"cold_bytes", report_.bytes.load(std::memory_order_relaxed)},
                           {"cold_prep_mean_us", static_cast<double>(cold_ns_sum) /
                                                     static_cast<double>(n_threads_) / 1e3},
                           {"cold_prep_max_us", static_cast<double>(cold_ns_max) / 1e3},
                           {"pre_trial_prep_max_us", static_cast<double>(pre_ns_max) / 1e3},
                           {"huge_pages", report_.huge_bytes.load(std::memory_order_relaxed) > 0},
                           {"huge_page_bytes", report_.huge_bytes.load(std::memory_order_relaxed)},
                           {"pinned", cfg_.pin},
                           {"cpus", assigned_},
                           {"interleaved", park_},
                           {"warmup_trials", cfg_.warmup_trials},
                           {"warmup_ms", cfg_.warmup_ms},
                           {"warmups_run", warmups_done_},
                           {"trial_ms", cfg_.trial_ms},
                           {"spin_ms", cfg_.spin_ms},
                           {"seed", cfg_.seed},
                           // "ops" is the work-unit count summed over threads. What a unit is
                           // depends on the metric and is spelled out next to it rather than left
                           // to the reader: bytes for a bandwidth metric, dependent loads for a
                           // latency chase, completed I/Os for IOPS.
                           {"ops", total_ops},
                           {"unit_of_work", to_string(work_unit_of(metrics_.front()))},
                           {"batch_ops", report_.batch_units.load(std::memory_order_relaxed)},
                           {"clock_res_ns", clock_.resolution_ns},
                           {"clock_call_ns", clock_.mean_call_ns},
                           {"start_spread_us", spread_us},
                           {"late_start", spread_us > kLateTrialUs},
                           {"release_to_first_start_us",
                            static_cast<double>(ns_between(t_release_, first_start)) / 1e3},
                           {"per_thread_ops_per_s", per_thread_ops},
                           {"per_thread_cpu", per_thread_cpu},
                           {"per_thread_start_us", per_thread_start_us},
                           {"pin_violations", pin_violations},
                           {"pin_failures", pin_failures}};
        {
            // Whatever this workload wants to say about itself: buffer sizes, the chase
            // length, the file it opened. Merged rather than nested so the dashboard and
            // `bench report` can read one flat params object.
            std::lock_guard lock{report_.mu};
            for (const auto& [k, v] : report_.extra.items())
                params[k] = v;
        }
        if (lat.samples > 0) {
            params["latency_samples"] = lat.samples;
            params["latency_min_us"] = lat.min_us;
            params["latency_max_us"] = lat.max_us;
            params["latency_overflow"] = lat.overflow;
        }

        const auto stamp = utc_now_rfc3339();
        for (std::size_t mi = 0; mi < metrics_.size(); ++mi) {
            const Metric m = metrics_[mi];
            Result r;
            r.workload = kind_;
            r.thread_count = n_threads_;
            r.working_set_bytes = working_set_;
            r.metric = m;
            r.value = value_rule_of(m) == ValueRule::latency_p99_us
                          ? lat.p99_us
                          : value_from_units(m, total_ops, ns, n_threads_);
            r.trial = warmup ? -warmups_done_ : timed_done_ - 1; // negative while warming up
            r.timestamp = stamp;
            r.duration_ns = ns == 0 ? 1 : ns;
            r.params = params;
            if (progress)
                progress(r, warmup);
            if (!warmup) {
                values_[mi].push_back(r.value);
                results_.push_back(std::move(r));
            }
        }
        if (!warmup && spread_us > kLateTrialUs)
            ++late_trials_;
    }

    // Results are buffered in the session rather than appended straight to the envelope,
    // for two reasons: an interleaved run must still write its results grouped by
    // configuration, and a configuration whose clock canary fails is thrown away whole.
    void emit(RunEnvelope& out, const ClockCheck& canary_start, const ClockCheck& canary_end,
              int attempts) override {
        if (results_.empty())
            return;
        for (auto& r : results_)
            out.results.push_back(std::move(r));
        results_.clear();

        // One summary per configuration per metric, over the timed trials that actually ran.
        // Statistics are defined in stats.hpp (sample stddev, numpy-linear percentiles).
        for (std::size_t mi = 0; mi < metrics_.size(); ++mi) {
            if (values_[mi].empty())
                continue;
            const SummaryStats st = summarize(values_[mi]);
            Summary sum;
            sum.workload = kind_;
            sum.metric = metrics_[mi];
            sum.thread_count = n_threads_;
            sum.working_set_bytes = working_set_;
            sum.n = static_cast<int>(st.n);
            sum.mean = st.mean;
            sum.median = st.median;
            sum.stddev = st.stddev;
            sum.cov = st.cov;
            sum.min = st.min;
            sum.p5 = st.p5;
            sum.p95 = st.p95;
            sum.max = st.max;
            sum.mad = st.mad;
            // M2.5 reporting: how many trials had a late worker, and what the host-contention
            // canary read before and after. Reported, never used to drop a trial.
            sum.late_trials = late_trials_;
            sum.canary_attempts = attempts;
            sum.clock_call_ns_start = canary_start.mean_call_ns;
            sum.clock_call_ns_end = canary_end.mean_call_ns;
            out.summary.push_back(sum);
        }
    }

private:
    const RunConfig& cfg_;
    Workload kind_;
    std::vector<Metric> metrics_;
    int n_threads_;
    std::uint64_t working_set_;
    int l3_kb_;
    const ClockCheck& clock_;
    bool park_;
    WorkloadContext ctx_;

    Clock::time_point t_release_{}, t_done_{};
    SpinBarrier start_b_, end_b_;
    std::latch ready_;
    std::vector<Slot> slots_;
    WorkerFailure failure_;
    WorkerReport report_;
    Gate gate_;
    std::vector<int> assigned_;
    std::vector<std::jthread> threads_;

    int warmups_done_ = 0, timed_done_ = 0, late_trials_ = 0;
    std::uint64_t active_ns_ = 0;
    std::vector<std::vector<double>> values_; // one per metric, parallel to metrics_
    std::vector<Result> results_;
};

} // namespace

void request_abort() noexcept {
    g_abort.store(true, std::memory_order_relaxed);
}
bool abort_requested() noexcept {
    return g_abort.load(std::memory_order_relaxed);
}
void clear_abort() noexcept {
    g_abort.store(false, std::memory_order_relaxed);
}

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine,
                           std::vector<std::string> argv, const ProgressFn& progress) {
    if (cfg.trials < 1)
        throw std::invalid_argument("trials must be >= 1");
    if (cfg.warmup_trials < 0)
        throw std::invalid_argument("warmup_trials must be >= 0");
    if (cfg.warmup_ms < 0)
        throw std::invalid_argument("warmup_ms must be >= 0");
    if (cfg.trial_ms < 1)
        throw std::invalid_argument("trial_ms must be >= 1");
    if (cfg.max_seconds < 0 || !std::isfinite(cfg.max_seconds))
        throw std::invalid_argument("max_seconds must be >= 0 and finite");
    for (int n : cfg.thread_counts)
        if (n < 1)
            throw std::invalid_argument("thread_count must be >= 1");
    for (int c : cfg.cpus)
        if (c < 0)
            throw std::invalid_argument("cpu ids must be >= 0");

    // `--workload mem_bw` means "all three of its metrics"; an explicit metric list wins.
    std::vector<Metric> metrics = cfg.metrics;
    if (metrics.empty())
        for (Workload w : cfg.workloads)
            for (Metric m : metrics_of(w))
                metrics.push_back(m);

    RunEnvelope run;
    run.run_id = new_run_id();
    run.started_at = utc_now_rfc3339();
    run.machine = machine;
    run.argv = std::move(argv);

    const int logical = machine.logical_cpus > 0
                            ? machine.logical_cpus
                            : static_cast<int>(std::thread::hardware_concurrency());
    const std::vector<int> cpu_list =
        cfg.cpus.empty() ? default_cpu_list(std::max(logical, 1)) : cfg.cpus;

    const ClockCheck clock = clock_selftest(kCanaryMaxSamples, kCanaryWindow);
    if (!clock.ok)
        std::cerr << std::format("bench: warning: clock self-test failed (mean now() = {:.1f} ns, "
                                 "monotonic = {}); results may be unreliable\n",
                                 clock.mean_call_ns, clock.monotonic);
    if (cfg.verbose) {
        std::cerr << std::format(
            "clock: resolution {} ns, now() mean {:.1f} ns, max {} ns, monotonic {}\n",
            clock.resolution_ns, clock.mean_call_ns, clock.max_call_ns, clock.monotonic);
        if (cfg.pin) {
            std::cerr << "pin: cpu list";
            for (int c : cpu_list)
                std::cerr << ' ' << c;
            std::cerr << '\n';
        }
    }

    busy_spin(std::chrono::milliseconds(cfg.spin_ms));

    // The safety cap covers the whole run, measured from after the frequency-ramp spin.
    // A zero time_point means "no deadline".
    const auto deadline = cfg.max_seconds > 0
                              ? Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(cfg.max_seconds))
                              : Clock::time_point{};

    // Every (metric, thread count, working set) the run will cover, in order.
    //
    // A configuration is per *metric*, not per workload: mem_bw owns three metrics and a
    // trial that mixed reads and writes would report neither. The exception is a metric
    // that is a different view of the same trial - disk_rand_read_p99_us is the percentile
    // of the very reads that disk_rand_read_iops counts - which rides along in the same
    // session instead of paying for a second run of the same work.
    struct ConfigSpec {
        Workload kind;
        std::vector<Metric> metrics;
        int threads;
        std::uint64_t working_set;
    };
    std::vector<ConfigSpec> specs;
    for (Metric metric : metrics) {
        const Workload kind = workload_of(metric);
        if (!is_implemented(kind))
            throw std::runtime_error(
                std::format("workload '{}' is not implemented yet", to_string(kind)));
        std::vector<Metric> group{metric};
        for (Metric rider : riders_of(metric))
            group.push_back(rider);
        for (int threads : cfg.thread_counts)
            for (std::uint64_t ws : cfg.working_sets)
                specs.push_back({kind, group, threads, effective_working_set(kind, ws)});
    }

    // Run-wide accounting for workloads that write to disk (M4.1). Shared by every session.
    std::atomic<std::uint64_t> bytes_written{0};

    auto make_session = [&](const ConfigSpec& sp, bool park) -> std::unique_ptr<ISession> {
        auto build = [&]<class W>() -> std::unique_ptr<ISession> {
            return std::make_unique<ConfigSession<W>>(cfg, sp.kind, sp.metrics, sp.threads,
                                                      sp.working_set, machine.l3_kb, clock,
                                                      cpu_list, park, &bytes_written);
        };
        switch (sp.kind) {
        case Workload::cpu_int:
            return build.template operator()<CpuIntWorkload>();
        case Workload::cpu_fp:
            return build.template operator()<CpuFpWorkload>();
        case Workload::cpu_hash:
            return build.template operator()<CpuHashWorkload>();
        case Workload::mem_bw:
            return build.template operator()<MemBwWorkload>();
        default:
            return build.template operator()<MemLatencyWorkload>();
        }
    };

    auto should_stop = [&](StopReason& out_reason) {
        if (g_abort.load(std::memory_order_relaxed)) {
            out_reason = StopReason::aborted;
            return true;
        }
        // The write budget is a wear limit, not a measurement rule: it stops the run at a
        // trial boundary, exactly like --max-seconds, so every trial that did run is a whole
        // honest trial rather than a truncated one.
        if (cfg.opts.write_budget_bytes > 0 &&
            bytes_written.load(std::memory_order_relaxed) >= cfg.opts.write_budget_bytes) {
            out_reason = StopReason::write_budget;
            return true;
        }
        if (deadline != Clock::time_point{} && Clock::now() >= deadline) {
            out_reason = StopReason::timed_out;
            return true;
        }
        return false;
    };

    // What counts as a contended host.
    //
    // M1.2's rule is an absolute "mean now() > 100 ns", which is a fact about a release
    // build on a sane clocksource - a quiet release build here reads ~14 ns. It is not a
    // fact about every build: under AddressSanitizer every now() call is instrumented and
    // an idle machine reads ~144 ns, so the absolute rule declares the host contended
    // before a single trial has run and then spends the whole time budget re-running
    // configurations that were never contaminated. Making the bar relative to a startup
    // reading does not work either - that reading is taken on the same possibly-busy host,
    // and at 220 spinners it came out at 8 us and blinded the canary exactly when the host
    // was worst.
    //
    // So the signal is the *shape* of the sample, not its scale. Between preemptions a
    // now() call costs whatever this build's calls cost, so the cheapest delta in the
    // window is the build's own overhead; contention shows up as a handful of calls that
    // took thousands of times longer, which moves the mean and not the minimum. The ratio
    // of the two is ~1 on any quiet machine, instrumented or not, and tens to thousands on
    // a contended one, with no constant to calibrate per build.
    auto canary_clean = [](const ClockCheck& c) {
        return c.monotonic && c.contention_ratio() <= kCanaryMaxRatio;
    };
    if (cfg.verbose)
        std::cerr << std::format(
            "canary: window {} ms, trips at mean/min > {:.0f} (startup mean {:.1f} ns, "
            "min {} ns, ratio {:.2f})\n",
            kCanaryWindow.count(), kCanaryMaxRatio, clock.mean_call_ns, clock.min_call_ns,
            clock.contention_ratio());

    StopReason stop = StopReason::none;
    if (cfg.interleave) {
        // All configurations alive at once, one trial each per round. Slow drifts - a
        // warming package, a background process that lasts a minute - then land on every
        // configuration in the same proportion instead of on whichever ran last.
        //
        // The clock canary is recorded but not acted on here: "re-run this configuration"
        // has no meaning when the configurations are braided together. Sequential runs get
        // the re-run rule; that is the trade the flag makes, and it is why it is off by
        // default.
        const ClockCheck canary_start = clock_selftest(kCanaryMaxSamples, kCanaryWindow);
        std::vector<std::unique_ptr<ISession>> sessions;
        sessions.reserve(specs.size());
        for (const auto& sp : specs) {
            sessions.push_back(make_session(sp, /*park=*/true));
            sessions.back()->prepare();
            sessions.back()->start();
        }
        bool any = true;
        while (any && stop == StopReason::none) {
            any = false;
            for (auto& sess : sessions) {
                if (should_stop(stop))
                    break;
                if (!sess->wants_more_trials())
                    continue;
                sess->run_one_trial(progress);
                any = true;
            }
        }
        for (auto& sess : sessions)
            sess->stop();
        const ClockCheck canary_end = clock_selftest(kCanaryMaxSamples, kCanaryWindow);
        for (auto& sess : sessions)
            sess->emit(run, canary_start, canary_end, 1);
    } else {
        for (const auto& sp : specs) {
            // Pre-declared re-run rule (M1.2 finding, M2.5 pitfall): the clock self-test is
            // a host-contention canary. If back-to-back steady_clock::now() calls are slow
            // at either end of a configuration, something outside this process was fighting
            // us for the machine, and the trials in between are contaminated. Throw the
            // whole configuration away and run it again. This is decided before any data is
            // seen, and it never inspects a trial value - it is not outlier trimming.
            int attempt = 0;
            for (;;) {
                ++attempt;
                const bool last_attempt = attempt > cfg.canary_retries;
                const ClockCheck canary_start = clock_selftest(kCanaryMaxSamples, kCanaryWindow);
                // Never spend the caller's time budget waiting for a quieter host: a run
                // under --max-seconds would rather have partial data than no data.
                const auto backoff = std::chrono::milliseconds(250);
                const bool room_to_retry =
                    deadline == Clock::time_point{} || Clock::now() + backoff < deadline;
                if (!canary_clean(canary_start) && !last_attempt && room_to_retry) {
                    if (cfg.verbose)
                        std::cerr << std::format(
                            "bench: {} t={} canary before: now() mean/min {:.1f} > {:.0f} "
                            "(mean {:.1f} ns, min {} ns); waiting and retrying (attempt {})\n",
                            to_string(sp.kind), sp.threads, canary_start.contention_ratio(),
                            kCanaryMaxRatio, canary_start.mean_call_ns, canary_start.min_call_ns,
                            attempt);
                    std::this_thread::sleep_for(backoff);
                    continue;
                }
                auto sess = make_session(sp, /*park=*/false);
                sess->prepare();
                sess->start();
                while (sess->wants_more_trials() && !should_stop(stop))
                    sess->run_one_trial(progress);
                sess->stop();
                const ClockCheck canary_end = clock_selftest(kCanaryMaxSamples, kCanaryWindow);
                // A run cut short by Ctrl-C or --max-seconds keeps what it has: re-running
                // it would not finish either, and the partial data is still honest.
                if (!canary_clean(canary_end) && !last_attempt && stop == StopReason::none &&
                    room_to_retry) {
                    if (cfg.verbose)
                        std::cerr << std::format(
                            "bench: {} t={} canary after: now() mean/min {:.1f} > {:.0f} "
                            "(mean {:.1f} ns, min {} ns); discarding {} trials and "
                            "re-running (attempt {})\n",
                            to_string(sp.kind), sp.threads, canary_end.contention_ratio(),
                            kCanaryMaxRatio, canary_end.mean_call_ns, canary_end.min_call_ns,
                            sess->timed_done(), attempt);
                    std::this_thread::sleep_for(backoff);
                    continue;
                }
                sess->emit(run, canary_start, canary_end, attempt);
                break;
            }
            if (stop != StopReason::none)
                break;
        }
    }

    if (stop == StopReason::aborted)
        std::cerr << "bench: interrupted; writing results collected so far\n";
    else if (stop == StopReason::timed_out)
        std::cerr << std::format("bench: --max-seconds {:g} reached; writing results collected "
                                 "so far ({} trials, {} summaries)\n",
                                 cfg.max_seconds, run.results.size(), run.summary.size());
    else if (stop == StopReason::write_budget)
        std::cerr << std::format("bench: --write-budget {} bytes spent ({} written); writing "
                                 "results collected so far ({} trials, {} summaries)\n",
                                 cfg.opts.write_budget_bytes,
                                 bytes_written.load(std::memory_order_relaxed), run.results.size(),
                                 run.summary.size());
    run.finished_at = utc_now_rfc3339();
    return run;
}

} // namespace bench
