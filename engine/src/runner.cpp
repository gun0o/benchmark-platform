#include "bench/runner.hpp"

#include "bench/timing.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/cpu_int.hpp"

#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>

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

namespace {

// One configuration = (workload, thread_count, working_set). Runs warmup + timed trials on
// the calling thread. thread_count is recorded but not yet honoured (M2.1).
template <WorkloadImpl W>
void run_config(const RunConfig& cfg, Workload kind, Metric metric, int thread_count,
                std::uint64_t working_set, const ClockCheck& clock, RunEnvelope& out,
                const ProgressFn& progress) {
    W w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = working_set, .seed = cfg.seed});

    const auto trial_len = std::chrono::milliseconds(cfg.trial_ms);
    const int total = cfg.warmup_trials + cfg.trials;
    for (int i = 0; i < total; ++i) {
        const bool warmup = i < cfg.warmup_trials;

        // Timed region: whole batches until trial_len has elapsed. The clock is read once
        // per batch (~1M ops), never inside the kernel loop.
        std::uint64_t ops = 0;
        std::uint64_t batches = 0;
        const auto t0 = Clock::now();
        auto now = t0;
        do {
            ops += w.run_batch();
            ++batches;
            now = Clock::now();
        } while (now - t0 < trial_len);
        const std::uint64_t ns = ns_between(t0, now);

        Result r;
        r.workload = kind;
        r.thread_count = thread_count;
        r.working_set_bytes = working_set;
        r.metric = metric;
        r.value = static_cast<double>(ops) / (static_cast<double>(ns) / 1e9);
        r.trial = i - cfg.warmup_trials; // negative while warming up, 0-based once timed
        r.timestamp = utc_now_rfc3339();
        r.duration_ns = ns == 0 ? 1 : ns;
        r.params = json{{"cold", "none"},
                        {"pinned", false},
                        {"warmup_trials", cfg.warmup_trials},
                        {"trial_ms", cfg.trial_ms},
                        {"spin_ms", cfg.spin_ms},
                        {"seed", cfg.seed},
                        {"ops", ops},
                        {"batches", batches},
                        {"batch_ops", ops / (batches == 0 ? 1 : batches)},
                        {"clock_res_ns", clock.resolution_ns},
                        {"clock_call_ns", clock.mean_call_ns}};
        if (progress)
            progress(r, warmup);
        if (!warmup)
            out.results.push_back(std::move(r));
    }
    w.teardown();
}

} // namespace

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine,
                           std::vector<std::string> argv, const ProgressFn& progress) {
    if (cfg.trials < 1)
        throw std::invalid_argument("trials must be >= 1");
    if (cfg.warmup_trials < 0)
        throw std::invalid_argument("warmup_trials must be >= 0");
    if (cfg.trial_ms < 1)
        throw std::invalid_argument("trial_ms must be >= 1");

    RunEnvelope run;
    run.run_id = new_run_id();
    run.started_at = utc_now_rfc3339();
    run.machine = machine;
    run.argv = std::move(argv);

    const ClockCheck clock = clock_selftest();
    if (!clock.ok)
        std::cerr << std::format("bench: warning: clock self-test failed (mean now() = {:.1f} ns, "
                                 "monotonic = {}); results may be unreliable\n",
                                 clock.mean_call_ns, clock.monotonic);
    if (cfg.verbose)
        std::cerr << std::format(
            "clock: resolution {} ns, now() mean {:.1f} ns, max {} ns, monotonic {}\n",
            clock.resolution_ns, clock.mean_call_ns, clock.max_call_ns, clock.monotonic);

    busy_spin(std::chrono::milliseconds(cfg.spin_ms));

    for (Workload kind : cfg.workloads) {
        for (int threads : cfg.thread_counts) {
            for (std::uint64_t ws : cfg.working_sets) {
                switch (kind) {
                case Workload::cpu_int:
                    run_config<CpuIntWorkload>(cfg, kind, Metric::cpu_int_ops, threads, ws, clock,
                                               run, progress);
                    break;
                default:
                    throw std::runtime_error(
                        std::format("workload '{}' is not implemented yet", to_string(kind)));
                }
            }
        }
    }
    run.finished_at = utc_now_rfc3339();
    return run;
}

} // namespace bench
