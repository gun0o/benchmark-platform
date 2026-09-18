#include "bench/runner.hpp"

#include <format>
#include <stdexcept>

#include "bench/timing.hpp"
#include "bench/workload.hpp"
#include "bench/workloads/cpu_int.hpp"

namespace bench {

const std::vector<WorkloadDesc>& workload_registry() {
    static const std::vector<WorkloadDesc> kRegistry = {
        {Workload::cpu_int, {Metric::cpu_int_ops}, "64-bit integer mul/shift/xor/add on 8 lanes", true},
        {Workload::cpu_fp, {Metric::cpu_fp_ops}, "double-precision FMA on 8 lanes", false},
        {Workload::cpu_hash, {Metric::cpu_hash_ops}, "xxHash-style 64-byte block hashing", false},
        {Workload::mem_bw, {Metric::mem_read_bw, Metric::mem_write_bw, Metric::mem_copy_bw},
         "streaming read / write / copy over a per-thread buffer", false},
        {Workload::mem_latency, {Metric::mem_latency}, "dependent-load pointer chase (Sattolo cycle)", false},
        {Workload::disk_seq, {Metric::disk_seq_read_bw, Metric::disk_seq_write_bw},
         "1 MiB O_DIRECT sequential read / write", false},
        {Workload::disk_rand,
         {Metric::disk_rand_read_iops, Metric::disk_rand_write_iops, Metric::disk_rand_read_p99_us},
         "4 KiB O_DIRECT random read / write, QD = threads", false},
    };
    return kRegistry;
}

namespace {

// M1.1: one batch per trial on the calling thread. thread_count is recorded but not yet
// honoured (M2.1). trial_ms is recorded but not yet honoured (M1.2).
template <WorkloadImpl W>
void run_config(const RunConfig& cfg, Workload kind, Metric metric, int thread_count,
                std::uint64_t working_set, RunEnvelope& out, const ProgressFn& progress) {
    W w;
    w.setup(WorkloadContext{.thread_index = 0, .working_set_bytes = working_set, .seed = cfg.seed});
    for (int trial = 0; trial < cfg.trials; ++trial) {
        const auto t0 = Clock::now();
        const std::uint64_t ops = w.run_batch();
        const auto t1 = Clock::now();
        const std::uint64_t ns = ns_between(t0, t1);

        Result r;
        r.workload = kind;
        r.thread_count = thread_count;
        r.working_set_bytes = working_set;
        r.metric = metric;
        r.value = static_cast<double>(ops) / (static_cast<double>(ns) / 1e9);
        r.trial = trial;
        r.timestamp = utc_now_rfc3339();
        r.duration_ns = ns == 0 ? 1 : ns;
        r.params = json{{"cold", "none"}, {"pinned", false}, {"warmup_trials", 0},
                        {"trial_ms", cfg.trial_ms}, {"seed", cfg.seed}, {"ops", ops}};
        if (progress) progress(r);
        out.results.push_back(std::move(r));
    }
    w.teardown();
}

} // namespace

RunEnvelope run_benchmarks(const RunConfig& cfg, const MachineInfo& machine, std::vector<std::string> argv,
                           const ProgressFn& progress) {
    RunEnvelope run;
    run.run_id = new_run_id();
    run.started_at = utc_now_rfc3339();
    run.machine = machine;
    run.argv = std::move(argv);

    for (Workload kind : cfg.workloads) {
        for (int threads : cfg.thread_counts) {
            for (std::uint64_t ws : cfg.working_sets) {
                switch (kind) {
                case Workload::cpu_int:
                    run_config<CpuIntWorkload>(cfg, kind, Metric::cpu_int_ops, threads, ws, run, progress);
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
