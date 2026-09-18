// The C++ enum tables and schema/benchmark-result.schema.json must agree exactly.
#include "bench/metric.hpp"
#include "bench/result.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <string>

using namespace bench;

static json load_schema() {
    std::ifstream f{std::filesystem::path{BENCH_SCHEMA_DIR} / "benchmark-result.schema.json"};
    EXPECT_TRUE(f.good()) << "schema file not found";
    return json::parse(f);
}

template <class E, std::size_t N> static std::set<std::string> names(const std::array<E, N>& all) {
    std::set<std::string> s;
    for (E e : all)
        s.emplace(to_string(e));
    return s;
}

static std::set<std::string> enum_values(const json& def) {
    std::set<std::string> s;
    for (const auto& v : def.at("enum"))
        s.insert(v.get<std::string>());
    return s;
}

TEST(MetricSchema, EnumsMatchSchemaFile) {
    const json schema = load_schema();
    EXPECT_EQ(names(kAllWorkloads), enum_values(schema["$defs"]["workload"]));
    EXPECT_EQ(names(kAllMetrics), enum_values(schema["$defs"]["metric"]));
    EXPECT_EQ(names(kAllUnits), enum_values(schema["$defs"]["unit"]));
}

TEST(MetricSchema, UnitAndWorkloadConditionalsMatch) {
    const json schema = load_schema();
    std::set<std::string> covered;
    for (const auto& clause : schema["$defs"]["result_core"]["allOf"]) {
        const std::string metric_name = clause["if"]["properties"]["metric"]["const"];
        const auto m = parse_metric(metric_name);
        ASSERT_TRUE(m.has_value()) << metric_name;
        EXPECT_EQ(clause["then"]["properties"]["unit"]["const"], to_string(unit_of(*m)))
            << metric_name;
        EXPECT_EQ(clause["then"]["properties"]["workload"]["const"], to_string(workload_of(*m)))
            << metric_name;
        covered.insert(metric_name);
    }
    EXPECT_EQ(covered, names(kAllMetrics)) << "every metric needs an if/then clause";
}
