#include "bench/runner.hpp"

#include "bench/sync.hpp"
#include "bench/timing.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/cpu_int.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <latch>
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
        {Workload::cpu_fp, {Metric::cpu_fp_ops}, "double-precision FMA on 8 lanes", false},
        {Workload::cpu_hash, {Metric::cpu_hash_ops}, "xxHash-style 64-byte block hashing", false},
        {Workload::mem_bw,
         {Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw},
         "streaming read / write / copy over a per-thread buffer",
         false},
        {Workload::mem_latency,
         {Metric::mem_latency},
         "dependent-load pointer chase (Sattolo cycle)",
         false},
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

namespace {

std::atomic<bool> g_abort{false};

// One per worker. 128 bytes so two workers' slots never share a cache line (or an
// adjacent-line prefetch pair). Only the owning worker writes it during a trial; the main
// thread reads it after the end barrier.
struct alignas(128) Slot {
    std::uint64_t ops = 0;
    std::uint64_t batches = 0;
    Clock::time_point start{};
    Clock::time_point end{};
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

int pin_current_thread(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

template <WorkloadImpl W>
void worker(std::stop_token st, int index, const RunConfig& cfg, std::uint64_t working_set,
            SpinBarrier& start_b, SpinBarrier& end_b, std::latch& ready, Slot& slot,
            WorkerFailure& failure) {
    if (slot.assigned_cpu >= 0)
        slot.pin_errno = pin_current_thread(slot.assigned_cpu);

    W w;
    bool ok = true;
    try {
        // Allocation and first-touch happen here, on the worker's own thread, never inside
        // the timed region.
        w.setup(WorkloadContext{
            .thread_index = index, .working_set_bytes = working_set, .seed = cfg.seed});
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
    for (;;) {
        // (M2.3: cold-cache preparation goes here, before the barrier.)
        start_b.arrive_and_wait();
        if (st.stop_requested())
            break; // main requested stop before arriving: consistent view

        // Every write to `slot` happens between the start and end barriers, so the main
        // thread's reads after the end barrier never race with the next trial's writes.
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
        slot.cpu_end = sched_getcpu();
        end_b.arrive_and_wait();
    }
    if (ok)
        w.teardown();
}

template <WorkloadImpl W>
bool run_config(const RunConfig& cfg, Workload kind, Metric metric, int n_threads,
                std::uint64_t working_set, const ClockCheck& clock,
                const std::vector<int>& cpu_list, RunEnvelope& out, const ProgressFn& progress) {
    Clock::time_point t_release{}, t_done{};
    SpinBarrier start_b(n_threads + 1, [&t_release] { t_release = Clock::now(); });
    SpinBarrier end_b(n_threads + 1, [&t_done] { t_done = Clock::now(); });
    std::latch ready(n_threads);
    std::vector<Slot> slots(static_cast<std::size_t>(n_threads));
    WorkerFailure failure;

    std::vector<int> assigned;
    for (int i = 0; i < n_threads; ++i) {
        const int cpu = cfg.pin ? cpu_list[static_cast<std::size_t>(i) % cpu_list.size()] : -1;
        slots[static_cast<std::size_t>(i)].assigned_cpu = cpu;
        if (cfg.pin)
            assigned.push_back(cpu);
    }

    std::vector<std::jthread> threads;
    threads.reserve(static_cast<std::size_t>(n_threads));
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker<W>, i, std::cref(cfg), working_set, std::ref(start_b),
                             std::ref(end_b), std::ref(ready),
                             std::ref(slots[static_cast<std::size_t>(i)]), std::ref(failure));
    auto stop_workers = [&] {
        for (auto& th : threads)
            th.request_stop();
        start_b.arrive_and_wait(); // releases the workers into their stop check
    };

    // Fail fast if any worker could not set up. Workers that failed still take part in the
    // barriers (with zero ops) so the ones that succeeded are not left waiting forever;
    // we stop them at the first trial boundary.
    ready.wait();
    {
        std::lock_guard lock{failure.mu};
        if (!failure.message.empty()) {
            stop_workers();
            throw std::runtime_error(failure.message);
        }
    }

    // Warmup: at least warmup_trials trials AND at least warmup_ms of wall time. The time
    // floor matters on VMs, where the first ~250 ms of a configuration show multi-ms start
    // spreads while the hypervisor settles the now-busy vCPUs onto physical cores.
    bool completed = true;
    constexpr auto kMainPoll = std::chrono::microseconds{50};
    const auto warmup_floor = std::chrono::milliseconds(cfg.warmup_ms);
    const auto config_start = Clock::now();
    int warmups_done = 0, timed_done = 0;
    for (;;) {
        if (g_abort.load(std::memory_order_relaxed)) {
            completed = false;
            break;
        }
        const bool warmup =
            warmups_done < cfg.warmup_trials || (Clock::now() - config_start) < warmup_floor;
        if (!warmup && timed_done >= cfg.trials)
            break;

        start_b.arrive_and_wait(); // main is usually the last arriver: it runs the t_release stamp
        end_b.arrive_and_wait_polling(
            kMainPoll); // last *worker* arrives last here and stamps t_done
        if (warmup)
            ++warmups_done;
        else
            ++timed_done;

        std::uint64_t total_ops = 0;
        auto first_start = slots[0].start, last_start = slots[0].start;
        json per_thread_ops = json::array(), per_thread_cpu = json::array(),
             per_thread_start_us = json::array();
        int pin_violations = 0, pin_failures = 0;
        for (const Slot& s : slots) {
            total_ops += s.ops;
            first_start = std::min(first_start, s.start);
            last_start = std::max(last_start, s.start);
            const auto own_ns = ns_between(s.start, s.end);
            per_thread_ops.push_back(
                own_ns ? static_cast<double>(s.ops) / (static_cast<double>(own_ns) / 1e9) : 0.0);
            per_thread_cpu.push_back(s.cpu_end);
            per_thread_start_us.push_back(static_cast<double>(ns_between(t_release, s.start)) /
                                          1e3);
            if (s.assigned_cpu >= 0) {
                if (s.pin_errno != 0)
                    ++pin_failures;
                else if (s.cpu_start != s.assigned_cpu || s.cpu_end != s.assigned_cpu)
                    ++pin_violations;
            }
        }
        const std::uint64_t ns = ns_between(t_release, t_done);

        Result r;
        r.workload = kind;
        r.thread_count = n_threads;
        r.working_set_bytes = working_set;
        r.metric = metric;
        r.value = static_cast<double>(total_ops) / (static_cast<double>(ns) / 1e9);
        r.trial = warmup ? -warmups_done : timed_done - 1; // negative while warming up
        r.timestamp = utc_now_rfc3339();
        r.duration_ns = ns == 0 ? 1 : ns;
        r.params = json{
            {"cold", "none"},
            {"pinned", cfg.pin},
            {"cpus", assigned},
            {"warmup_trials", cfg.warmup_trials},
            {"warmup_ms", cfg.warmup_ms},
            {"warmups_run", warmups_done},
            {"trial_ms", cfg.trial_ms},
            {"spin_ms", cfg.spin_ms},
            {"seed", cfg.seed},
            {"ops", total_ops},
            {"batch_ops", W::kItersPerBatch * static_cast<std::uint64_t>(W::kLanes)},
            {"clock_res_ns", clock.resolution_ns},
            {"clock_call_ns", clock.mean_call_ns},
            {"start_spread_us", static_cast<double>(ns_between(first_start, last_start)) / 1e3},
            {"release_to_first_start_us",
             static_cast<double>(ns_between(t_release, first_start)) / 1e3},
            {"per_thread_ops_per_s", per_thread_ops},
            {"per_thread_cpu", per_thread_cpu},
            {"per_thread_start_us", per_thread_start_us},
            {"pin_violations", pin_violations},
            {"pin_failures", pin_failures}};
        if (progress)
            progress(r, warmup);
        if (!warmup)
            out.results.push_back(std::move(r));
    }
    stop_workers(); // jthread destructors then join
    return completed;
}

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
    for (int n : cfg.thread_counts)
        if (n < 1)
            throw std::invalid_argument("thread_count must be >= 1");
    for (int c : cfg.cpus)
        if (c < 0)
            throw std::invalid_argument("cpu ids must be >= 0");

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

    const ClockCheck clock = clock_selftest();
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

    bool completed = true;
    for (Workload kind : cfg.workloads) {
        for (int threads : cfg.thread_counts) {
            for (std::uint64_t ws : cfg.working_sets) {
                if (!completed)
                    break;
                switch (kind) {
                case Workload::cpu_int:
                    completed = run_config<CpuIntWorkload>(cfg, kind, Metric::cpu_int_ops, threads,
                                                           ws, clock, cpu_list, run, progress);
                    break;
                default:
                    throw std::runtime_error(
                        std::format("workload '{}' is not implemented yet", to_string(kind)));
                }
            }
        }
    }
    if (!completed)
        std::cerr << "bench: interrupted; writing results collected so far\n";
    run.finished_at = utc_now_rfc3339();
    return run;
}

} // namespace bench
