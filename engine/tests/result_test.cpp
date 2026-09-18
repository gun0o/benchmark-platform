#include "bench/result.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>

using namespace bench;

static RunEnvelope sample_run() {
    RunEnvelope run;
    run.run_id = new_run_id();
    run.started_at = utc_now_rfc3339();
    run.finished_at = utc_now_rfc3339();
    run.machine = MachineInfo{.id = std::string(32, 'a'),
                              .hostname = "h",
                              .cpu_model = "cpu",
                              .physical_cores = 4,
                              .logical_cpus = 8,
                              .l1d_kb = 48,
                              .l2_kb = 2048,
                              .l3_kb = 24576,
                              .memory_bytes = 1ull << 34,
                              .os = "os",
                              .kernel = "k",
                              .compiler = "g++ 13",
                              .compiler_flags = "-O3",
                              .engine_version = "0.1.0",
                              .engine_git_sha = "abc",
                              .virtualized = "none"};
    run.argv = {"bench", "run"};
    run.results.push_back(Result{.workload = Workload::cpu_int,
                                 .thread_count = 2,
                                 .working_set_bytes = 0,
                                 .metric = Metric::cpu_int_ops,
                                 .value = 1.5e9,
                                 .trial = 0,
                                 .timestamp = utc_now_rfc3339(),
                                 .duration_ns = 50'000'000,
                                 .params = json{{"cold", "none"}}});
    run.summary.push_back(Summary{.workload = Workload::cpu_int,
                                  .metric = Metric::cpu_int_ops,
                                  .thread_count = 2,
                                  .working_set_bytes = 0,
                                  .n = 1,
                                  .mean = 1.5e9,
                                  .median = 1.5e9,
                                  .stddev = 0,
                                  .cov = 0,
                                  .min = 1.5e9,
                                  .p5 = 1.5e9,
                                  .p95 = 1.5e9,
                                  .max = 1.5e9});
    return run;
}

TEST(Result, JsonRoundTrip) {
    const RunEnvelope a = sample_run();
    const json j = to_json(a);
    const RunEnvelope b = run_from_json(j);
    EXPECT_EQ(a, b);
    EXPECT_EQ(to_json(b), j);
}

TEST(Result, EmittedJsonPassesStructuralValidation) {
    EXPECT_TRUE(validate_run(to_json(sample_run())).empty());
}

TEST(Result, UnitIsDerivedFromMetric) {
    const json j = to_json(sample_run());
    EXPECT_EQ(j["results"][0]["unit"], "ops/s");
    EXPECT_FALSE(j["results"][0].contains("machine"));
}

TEST(Result, ValidationCatchesCommonMistakes) {
    json j = to_json(sample_run());
    j["results"][0].erase("unit");
    auto p = validate_run(j);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_NE(p[0].find("missing required 'unit'"), std::string::npos);

    j = to_json(sample_run());
    j["results"][0]["unit"] = "GB/s";
    p = validate_run(j);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_NE(p[0].find("'ops/s' expected"), std::string::npos);

    j = to_json(sample_run());
    j["results"][0]["machine"] = json::object();
    EXPECT_FALSE(validate_run(j).empty());

    j = to_json(sample_run());
    j["machine"]["id"] = "short";
    EXPECT_FALSE(validate_run(j).empty());

    j = to_json(sample_run());
    j["results"][0]["timestamp"] = "2026-09-18 15:47:01";
    EXPECT_FALSE(validate_run(j).empty());
}

TEST(Result, StrictParserRejectsWrongUnit) {
    json j = to_json(sample_run());
    j["results"][0]["unit"] = "ns";
    EXPECT_THROW(run_from_json(j), std::runtime_error);
}

TEST(Result, SchemaExampleParsesAndValidates) {
    std::ifstream f{std::filesystem::path{BENCH_SCHEMA_DIR} / "examples/run.json"};
    ASSERT_TRUE(f.good());
    const json j = json::parse(f);
    EXPECT_TRUE(validate_run(j).empty());
    const RunEnvelope run = run_from_json(j);
    EXPECT_EQ(run.results.size(), 3u);
    EXPECT_EQ(run.results[2].metric, Metric::mem_latency);
}

TEST(Result, RunIdIsUuidV4) {
    const std::regex re{"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
    for (int i = 0; i < 100; ++i)
        EXPECT_TRUE(std::regex_match(new_run_id(), re)) << new_run_id();
    EXPECT_NE(new_run_id(), new_run_id());
}

TEST(Result, TimestampIsRfc3339UtcMicroseconds) {
    const std::regex re{"^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{6}Z$"};
    EXPECT_TRUE(std::regex_match(utc_now_rfc3339(), re)) << utc_now_rfc3339();
}
