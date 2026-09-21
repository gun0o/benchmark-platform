set(INSTALL_GTEST OFF CACHE INTERNAL "")
set(BUILD_GMOCK OFF CACHE INTERNAL "")
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

set(BENCH_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(BENCH_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../schema")

# bench_test(<name> [LABELS <label>...]) ; tests labelled "perf" assert timing ratios and
# can be skipped on noisy CI runners with `ctest -LE perf`.
function(bench_test name)
  cmake_parse_arguments(BT "" "" "LABELS" ${ARGN})
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE bench_core GTest::gtest_main)
  target_compile_definitions(${name} PRIVATE
    BENCH_FIXTURE_DIR="${BENCH_FIXTURE_DIR}"
    BENCH_SCHEMA_DIR="${BENCH_SCHEMA_DIR}")
  if(BT_LABELS)
    gtest_discover_tests(${name} PROPERTIES LABELS "${BT_LABELS}")
  else()
    gtest_discover_tests(${name})
  endif()
endfunction()

bench_test(metric_test)
bench_test(sha256_test)
bench_test(sysinfo_test)
bench_test(result_test)
bench_test(metric_schema_test)
bench_test(timing_test)
bench_test(stats_test)
bench_test(runner_test)
bench_test(cache_test)
bench_test(cpu_workloads_test)
bench_test(variance_test)
bench_test(false_sharing_test LABELS perf)
bench_test(barrier_sync_test LABELS perf)
bench_test(cold_cache_test LABELS perf)
bench_test(cpu_scaling_test LABELS perf)
