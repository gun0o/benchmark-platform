#include "bench/result.hpp"

#include <chrono>
#include <format>
#include <random>
#include <stdexcept>

namespace bench {
namespace {

template <class T> T get_required(const json& j, const char* key, const char* where) {
    const auto it = j.find(key);
    if (it == j.end())
        throw std::runtime_error(std::format("{}: missing '{}'", where, key));
    try {
        return it->get<T>();
    } catch (const json::exception& e) {
        throw std::runtime_error(std::format("{}.{}: {}", where, key, e.what()));
    }
}

} // namespace

json to_json(const MachineInfo& m) {
    return json{{"id", m.id},
                {"hostname", m.hostname},
                {"cpu_model", m.cpu_model},
                {"physical_cores", m.physical_cores},
                {"logical_cpus", m.logical_cpus},
                {"l1d_kb", m.l1d_kb},
                {"l2_kb", m.l2_kb},
                {"l3_kb", m.l3_kb},
                {"memory_bytes", m.memory_bytes},
                {"os", m.os},
                {"kernel", m.kernel},
                {"compiler", m.compiler},
                {"compiler_flags", m.compiler_flags},
                {"engine_version", m.engine_version},
                {"engine_git_sha", m.engine_git_sha},
                {"virtualized", m.virtualized}};
}

json to_json(const Result& r) {
    return json{{"workload", to_string(r.workload)},
                {"thread_count", r.thread_count},
                {"working_set_bytes", r.working_set_bytes},
                {"metric", to_string(r.metric)},
                {"value", r.value},
                {"unit", to_string(unit_of(r.metric))},
                {"trial", r.trial},
                {"timestamp", r.timestamp},
                {"duration_ns", r.duration_ns},
                {"params", r.params}};
}

json to_json(const Summary& s) {
    return json{{"workload", to_string(s.workload)},
                {"metric", to_string(s.metric)},
                {"thread_count", s.thread_count},
                {"working_set_bytes", s.working_set_bytes},
                {"n", s.n},
                {"mean", s.mean},
                {"median", s.median},
                {"stddev", s.stddev},
                {"cov", s.cov},
                {"min", s.min},
                {"p5", s.p5},
                {"p95", s.p95},
                {"max", s.max},
                {"mad", s.mad},
                {"late_trials", s.late_trials},
                {"canary_attempts", s.canary_attempts},
                {"clock_call_ns_start", s.clock_call_ns_start},
                {"clock_call_ns_end", s.clock_call_ns_end}};
}

json to_json(const RunEnvelope& run) {
    json results = json::array();
    for (const auto& r : run.results)
        results.push_back(to_json(r));
    json summary = json::array();
    for (const auto& s : run.summary)
        summary.push_back(to_json(s));
    return json{{"schema_version", run.schema_version}, {"run_id", run.run_id},
                {"started_at", run.started_at},         {"finished_at", run.finished_at},
                {"machine", to_json(run.machine)},      {"argv", run.argv},
                {"results", std::move(results)},        {"summary", std::move(summary)}};
}

MachineInfo machine_from_json(const json& j) {
    const char* w = "machine";
    MachineInfo m;
    m.id = get_required<std::string>(j, "id", w);
    m.hostname = get_required<std::string>(j, "hostname", w);
    m.cpu_model = get_required<std::string>(j, "cpu_model", w);
    m.physical_cores = get_required<int>(j, "physical_cores", w);
    m.logical_cpus = get_required<int>(j, "logical_cpus", w);
    m.l1d_kb = get_required<int>(j, "l1d_kb", w);
    m.l2_kb = get_required<int>(j, "l2_kb", w);
    m.l3_kb = get_required<int>(j, "l3_kb", w);
    m.memory_bytes = get_required<std::uint64_t>(j, "memory_bytes", w);
    m.os = get_required<std::string>(j, "os", w);
    m.kernel = get_required<std::string>(j, "kernel", w);
    m.compiler = get_required<std::string>(j, "compiler", w);
    m.compiler_flags = get_required<std::string>(j, "compiler_flags", w);
    m.engine_version = get_required<std::string>(j, "engine_version", w);
    m.engine_git_sha = get_required<std::string>(j, "engine_git_sha", w);
    m.virtualized = j.value("virtualized", "");
    return m;
}

Result result_from_json(const json& j) {
    const char* w = "result";
    Result r;
    const auto wl = parse_workload(get_required<std::string>(j, "workload", w));
    if (!wl)
        throw std::runtime_error("result.workload: unknown value");
    r.workload = *wl;
    r.thread_count = get_required<int>(j, "thread_count", w);
    r.working_set_bytes = get_required<std::uint64_t>(j, "working_set_bytes", w);
    const auto me = parse_metric(get_required<std::string>(j, "metric", w));
    if (!me)
        throw std::runtime_error("result.metric: unknown value");
    r.metric = *me;
    r.value = get_required<double>(j, "value", w);
    const auto un = parse_unit(get_required<std::string>(j, "unit", w));
    if (!un || *un != unit_of(r.metric))
        throw std::runtime_error(std::format("result.unit: expected '{}' for metric '{}'",
                                             to_string(unit_of(r.metric)), to_string(r.metric)));
    r.trial = get_required<int>(j, "trial", w);
    r.timestamp = get_required<std::string>(j, "timestamp", w);
    r.duration_ns = get_required<std::uint64_t>(j, "duration_ns", w);
    r.params = j.value("params", json::object());
    return r;
}

Summary summary_from_json(const json& j) {
    const char* w = "summary";
    Summary s;
    const auto wl = parse_workload(get_required<std::string>(j, "workload", w));
    const auto me = parse_metric(get_required<std::string>(j, "metric", w));
    if (!wl || !me)
        throw std::runtime_error("summary: unknown workload/metric");
    s.workload = *wl;
    s.metric = *me;
    s.thread_count = get_required<int>(j, "thread_count", w);
    s.working_set_bytes = get_required<std::uint64_t>(j, "working_set_bytes", w);
    s.n = get_required<int>(j, "n", w);
    s.mean = get_required<double>(j, "mean", w);
    s.median = get_required<double>(j, "median", w);
    s.stddev = get_required<double>(j, "stddev", w);
    s.cov = get_required<double>(j, "cov", w);
    s.min = get_required<double>(j, "min", w);
    s.p5 = get_required<double>(j, "p5", w);
    s.p95 = get_required<double>(j, "p95", w);
    s.max = get_required<double>(j, "max", w);
    // All optional: documents written before M2.2 / M2.5 predate them.
    s.mad = j.value("mad", 0.0);
    s.late_trials = j.value("late_trials", 0);
    s.canary_attempts = j.value("canary_attempts", 1);
    s.clock_call_ns_start = j.value("clock_call_ns_start", 0.0);
    s.clock_call_ns_end = j.value("clock_call_ns_end", 0.0);
    return s;
}

RunEnvelope run_from_json(const json& j) {
    const char* w = "run";
    RunEnvelope run;
    run.schema_version = get_required<int>(j, "schema_version", w);
    if (run.schema_version != 1)
        throw std::runtime_error(
            std::format("run.schema_version: unsupported {}", run.schema_version));
    run.run_id = get_required<std::string>(j, "run_id", w);
    run.started_at = get_required<std::string>(j, "started_at", w);
    run.finished_at = get_required<std::string>(j, "finished_at", w);
    run.machine = machine_from_json(get_required<json>(j, "machine", w));
    run.argv = get_required<std::vector<std::string>>(j, "argv", w);
    for (const auto& r : get_required<json>(j, "results", w))
        run.results.push_back(result_from_json(r));
    if (j.contains("summary"))
        for (const auto& s : j["summary"])
            run.summary.push_back(summary_from_json(s));
    return run;
}

std::string new_run_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng(), lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull; // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 10xx
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", hi >> 32, (hi >> 16) & 0xFFFF,
                       hi & 0xFFFF, lo >> 48, lo & 0xFFFFFFFFFFFFull);
}

std::string utc_now_rfc3339() {
    using namespace std::chrono;
    const auto now = floor<microseconds>(system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

} // namespace bench
