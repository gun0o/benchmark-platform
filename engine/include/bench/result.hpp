// Result / run-envelope structs and their JSON (de)serialization.
// Shape is defined by schema/benchmark-result.schema.json.
#pragma once

#include "bench/metric.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace bench {

using json = nlohmann::json;

struct MachineInfo {
    std::string id; // 32 hex chars, see machine_id()
    std::string hostname;
    std::string cpu_model;
    int physical_cores = 0;
    int logical_cpus = 0;
    int l1d_kb = 0;
    int l2_kb = 0;
    int l3_kb = 0;
    std::uint64_t memory_bytes = 0;
    std::string os;
    std::string kernel;
    std::string compiler;
    std::string compiler_flags;
    std::string engine_version;
    std::string engine_git_sha;
    std::string virtualized; // "none", "wsl2", ...

    bool operator==(const MachineInfo&) const = default;
};

struct Result {
    Workload workload{};
    int thread_count = 1;
    std::uint64_t working_set_bytes = 0;
    Metric metric{};
    double value = 0.0;
    int trial = 0;
    std::string timestamp; // RFC 3339 UTC, microseconds
    std::uint64_t duration_ns = 0;
    json params = json::object();

    bool operator==(const Result&) const = default;
};

struct Summary {
    Workload workload{};
    Metric metric{};
    int thread_count = 1;
    std::uint64_t working_set_bytes = 0;
    int n = 0;
    // stddev is the SAMPLE standard deviation (n-1); p5/p95/median use numpy's "linear"
    // percentile interpolation; cov = stddev / mean; mad = median(|x - median(x)|).
    double mean = 0, median = 0, stddev = 0, cov = 0, min = 0, p5 = 0, p95 = 0, max = 0, mad = 0;
    // M2.5 run-quality fields. They describe the conditions the trials were taken under,
    // not the trials themselves, and nothing here is ever used to drop a value.
    int late_trials = 0;            // timed trials whose worker start spread exceeded 1000 us
    int canary_attempts = 1;        // >1: an earlier attempt was discarded, host was contended
    double clock_call_ns_start = 0; // clock self-test before this configuration's trials
    double clock_call_ns_end = 0;   // ...and after

    bool operator==(const Summary&) const = default;
};

struct RunEnvelope {
    int schema_version = 1;
    std::string run_id;
    std::string started_at;
    std::string finished_at;
    MachineInfo machine;
    std::vector<std::string> argv;
    std::vector<Result> results;
    std::vector<Summary> summary;

    bool operator==(const RunEnvelope&) const = default;
};

json to_json(const MachineInfo& m);
json to_json(const Result& r);
json to_json(const Summary& s);
json to_json(const RunEnvelope& run);

// Strict parsers; throw std::runtime_error with a path-qualified message on malformed input.
MachineInfo machine_from_json(const json& j);
Result result_from_json(const json& j);
Summary summary_from_json(const json& j);
RunEnvelope run_from_json(const json& j);

// Structural validation mirroring the schema (required keys, enums, metric<->unit/workload
// consistency, no machine block inside a run result). Returns human-readable problems,
// empty when valid.
std::vector<std::string> validate_run(const json& j);

std::string new_run_id();      // UUID v4
std::string utc_now_rfc3339(); // e.g. 2026-09-18T15:47:00.123456Z

} // namespace bench
