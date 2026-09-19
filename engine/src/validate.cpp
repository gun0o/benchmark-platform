// Structural validation of a run document. Mirrors schema/benchmark-result.schema.json
// closely enough for `bench validate` to catch what the engine could emit wrongly; the
// JSON Schema file remains the source of truth (CI validates examples against it).
#include "bench/result.hpp"

#include <format>
#include <regex>

namespace bench {
namespace {

using Problems = std::vector<std::string>;

const std::regex kUuid{"^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
const std::regex kTimestamp{"^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d{1,9})?Z$"};
const std::regex kMachineId{"^[0-9a-f]{32}$"};

bool is_int(const json& v) {
    return v.is_number_integer();
}

void require(const json& j, const char* key, const std::string& where, Problems& out,
             bool (*pred)(const json&), const char* type_name) {
    const auto it = j.find(key);
    if (it == j.end()) {
        out.push_back(std::format("{}: missing required '{}'", where, key));
    } else if (!pred(*it)) {
        out.push_back(std::format("{}.{}: expected {}", where, key, type_name));
    }
}

void check_machine(const json& m, Problems& out) {
    const std::string w = "machine";
    if (!m.is_object()) {
        out.push_back("machine: expected object");
        return;
    }
    for (const char* k : {"id", "hostname", "cpu_model", "os", "kernel", "compiler",
                          "compiler_flags", "engine_version", "engine_git_sha"})
        require(m, k, w, out, [](const json& v) { return v.is_string(); }, "string");
    for (const char* k :
         {"physical_cores", "logical_cpus", "l1d_kb", "l2_kb", "l3_kb", "memory_bytes"})
        require(m, k, w, out, is_int, "integer");
    if (m.contains("id") && m["id"].is_string() &&
        !std::regex_match(m["id"].get<std::string>(), kMachineId))
        out.push_back("machine.id: must be 32 lowercase hex chars");
}

void check_result(const json& r, std::size_t i, Problems& out) {
    const std::string w = std::format("results[{}]", i);
    if (!r.is_object()) {
        out.push_back(w + ": expected object");
        return;
    }
    for (const char* k : {"schema_version", "run_id", "machine"})
        if (r.contains(k))
            out.push_back(std::format("{}: '{}' must not appear inside a run result", w, k));
    require(r, "workload", w, out, [](const json& v) { return v.is_string(); }, "string");
    require(r, "metric", w, out, [](const json& v) { return v.is_string(); }, "string");
    require(r, "unit", w, out, [](const json& v) { return v.is_string(); }, "string");
    require(r, "timestamp", w, out, [](const json& v) { return v.is_string(); }, "string");
    require(r, "value", w, out, [](const json& v) { return v.is_number(); }, "number");
    for (const char* k : {"thread_count", "working_set_bytes", "trial", "duration_ns"})
        require(r, k, w, out, is_int, "integer");

    std::optional<Metric> metric;
    if (r.contains("metric") && r["metric"].is_string()) {
        metric = parse_metric(r["metric"].get<std::string>());
        if (!metric)
            out.push_back(
                std::format("{}.metric: unknown '{}'", w, r["metric"].get<std::string>()));
    }
    if (r.contains("workload") && r["workload"].is_string()) {
        const auto wl = parse_workload(r["workload"].get<std::string>());
        if (!wl)
            out.push_back(
                std::format("{}.workload: unknown '{}'", w, r["workload"].get<std::string>()));
        else if (metric && *wl != workload_of(*metric))
            out.push_back(std::format("{}.workload: '{}' expected for metric '{}'", w,
                                      to_string(workload_of(*metric)), to_string(*metric)));
    }
    if (r.contains("unit") && r["unit"].is_string()) {
        const auto un = parse_unit(r["unit"].get<std::string>());
        if (!un)
            out.push_back(std::format("{}.unit: unknown '{}'", w, r["unit"].get<std::string>()));
        else if (metric && *un != unit_of(*metric))
            out.push_back(std::format("{}.unit: '{}' expected for metric '{}'", w,
                                      to_string(unit_of(*metric)), to_string(*metric)));
    }
    if (r.contains("timestamp") && r["timestamp"].is_string() &&
        !std::regex_match(r["timestamp"].get<std::string>(), kTimestamp))
        out.push_back(w + ".timestamp: must be RFC 3339 UTC with 'Z'");
    if (r.contains("thread_count") && is_int(r["thread_count"]) &&
        r["thread_count"].get<long long>() < 1)
        out.push_back(w + ".thread_count: must be >= 1");
    if (r.contains("trial") && is_int(r["trial"]) && r["trial"].get<long long>() < 0)
        out.push_back(w + ".trial: must be >= 0");
    if (r.contains("value") && r["value"].is_number() && !std::isfinite(r["value"].get<double>()))
        out.push_back(w + ".value: must be finite");
    // params is open-ended, but the keys the schema does pin down are checked here, because
    // a result claiming a cold mode the engine cannot perform is exactly the kind of quiet
    // lie this project is trying not to publish.
    if (r.contains("params") && r["params"].is_object()) {
        const json& p = r["params"];
        if (const auto it = p.find("cold"); it != p.end()) {
            if (!it->is_string())
                out.push_back(w + ".params.cold: expected string");
            else if (const auto c = it->get<std::string>();
                     c != "clflush" && c != "evict" && c != "none" && c != "n/a")
                out.push_back(std::format("{}.params.cold: unknown '{}'", w, c));
        }
        if (const auto it = p.find("huge_pages"); it != p.end() && !it->is_boolean())
            out.push_back(w + ".params.huge_pages: expected boolean");
    }
}

void check_summary(const json& s, std::size_t i, Problems& out) {
    const std::string w = std::format("summary[{}]", i);
    if (!s.is_object()) {
        out.push_back(w + ": expected object");
        return;
    }
    require(s, "workload", w, out, [](const json& v) { return v.is_string(); }, "string");
    require(s, "metric", w, out, [](const json& v) { return v.is_string(); }, "string");
    for (const char* k : {"thread_count", "working_set_bytes", "n"})
        require(s, k, w, out, is_int, "integer");
    for (const char* k : {"mean", "median", "stddev", "cov", "min", "p5", "p95", "max"})
        require(s, k, w, out, [](const json& v) { return v.is_number(); }, "number");

    std::optional<Metric> metric;
    if (s.contains("metric") && s["metric"].is_string()) {
        metric = parse_metric(s["metric"].get<std::string>());
        if (!metric)
            out.push_back(
                std::format("{}.metric: unknown '{}'", w, s["metric"].get<std::string>()));
    }
    if (s.contains("workload") && s["workload"].is_string()) {
        const auto wl = parse_workload(s["workload"].get<std::string>());
        if (!wl)
            out.push_back(
                std::format("{}.workload: unknown '{}'", w, s["workload"].get<std::string>()));
        else if (metric && *wl != workload_of(*metric))
            out.push_back(std::format("{}.workload: '{}' expected for metric '{}'", w,
                                      to_string(workload_of(*metric)), to_string(*metric)));
    }
    if (s.contains("n") && is_int(s["n"]) && s["n"].get<long long>() < 1)
        out.push_back(w + ".n: must be >= 1");
    for (const char* k : {"stddev", "cov", "mad"})
        if (s.contains(k) && s[k].is_number() && s[k].get<double>() < 0)
            out.push_back(std::format("{}.{}: must be >= 0", w, k));
    // Ordering invariants that any correct summary satisfies.
    auto num = [&](const char* k) {
        return s.contains(k) && s[k].is_number() ? s[k].get<double>() : 0.0;
    };
    if (s.contains("min") && s.contains("max") && num("min") > num("max"))
        out.push_back(w + ": min > max");
    if (s.contains("min") && s.contains("median") && num("min") > num("median"))
        out.push_back(w + ": min > median");
    if (s.contains("median") && s.contains("max") && num("median") > num("max"))
        out.push_back(w + ": median > max");
    if (s.contains("p5") && s.contains("p95") && num("p5") > num("p95"))
        out.push_back(w + ": p5 > p95");
}

} // namespace

std::vector<std::string> validate_run(const json& j) {
    Problems out;
    if (!j.is_object()) {
        out.push_back("run: expected object");
        return out;
    }
    const std::string w = "run";
    require(j, "schema_version", w, out, is_int, "integer");
    if (j.contains("schema_version") && is_int(j["schema_version"]) &&
        j["schema_version"].get<int>() != 1)
        out.push_back("run.schema_version: must be 1");
    require(j, "run_id", w, out, [](const json& v) { return v.is_string(); }, "string");
    if (j.contains("run_id") && j["run_id"].is_string() &&
        !std::regex_match(j["run_id"].get<std::string>(), kUuid))
        out.push_back("run.run_id: must be a lowercase UUID");
    for (const char* k : {"started_at", "finished_at"}) {
        require(j, k, w, out, [](const json& v) { return v.is_string(); }, "string");
        if (j.contains(k) && j[k].is_string() &&
            !std::regex_match(j[k].get<std::string>(), kTimestamp))
            out.push_back(std::format("run.{}: must be RFC 3339 UTC with 'Z'", k));
    }
    require(j, "argv", w, out, [](const json& v) { return v.is_array(); }, "array");
    require(j, "results", w, out, [](const json& v) { return v.is_array(); }, "array");
    if (j.contains("machine"))
        check_machine(j["machine"], out);
    else
        out.push_back("run: missing required 'machine'");
    if (j.contains("results") && j["results"].is_array())
        for (std::size_t i = 0; i < j["results"].size(); ++i)
            check_result(j["results"][i], i, out);
    if (j.contains("summary")) {
        if (!j["summary"].is_array())
            out.push_back("run.summary: expected array");
        else
            for (std::size_t i = 0; i < j["summary"].size(); ++i)
                check_summary(j["summary"][i], i, out);
    }
    return out;
}

} // namespace bench
