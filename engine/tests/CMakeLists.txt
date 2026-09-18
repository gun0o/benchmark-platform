set(INSTALL_GTEST OFF CACHE INTERNAL "")
set(BUILD_GMOCK OFF CACHE INTERNAL "")
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

set(BENCH_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(BENCH_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../schema")

function(bench_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE bench_core GTest::gtest_main)
  target_compile_definitions(${name} PRIVATE
    BENCH_FIXTURE_DIR="${BENCH_FIXTURE_DIR}"
    BENCH_SCHEMA_DIR="${BENCH_SCHEMA_DIR}")
  gtest_discover_tests(${name})
endfunction()

bench_test(metric_test)
bench_test(sha256_test)
bench_test(sysinfo_test)
bench_test(result_test)
bench_test(metric_schema_test)
