cmake_minimum_required(VERSION 3.22)
project(bench VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BENCH_BUILD_TESTS "Build unit tests" ON)
option(BENCH_NATIVE "Compile with -march=native (real numbers); OFF gives -march=x86-64-v3 (portable)" ON)
set(BENCH_SANITIZER "" CACHE STRING "Sanitizer to enable: '', address, thread")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- third-party (header-only / test) via FetchContent -------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_SystemInclude ON CACHE INTERNAL "")   # keep -Wconversion out of the library
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)

set(CLI11_PRECOMPILED OFF CACHE INTERNAL "")
set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(CLI11_INSTALL OFF CACHE INTERNAL "")
FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz)

FetchContent_MakeAvailable(nlohmann_json CLI11)

# CLI11 does not mark its include dir SYSTEM; do it so our -Werror set does not apply to it.
get_target_property(_cli11_inc CLI11 INTERFACE_INCLUDE_DIRECTORIES)
add_library(cli11_system INTERFACE)
target_include_directories(cli11_system SYSTEM INTERFACE ${_cli11_inc})

# ---- build metadata baked into the binary ----------------------------------------------
find_package(Git QUIET)
set(BENCH_GIT_SHA "unknown")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
  if(_rc EQUAL 0 AND NOT _sha STREQUAL "")
    set(BENCH_GIT_SHA "${_sha}")
  endif()
endif()

if(BENCH_NATIVE)
  set(BENCH_ARCH_FLAG -march=native)
else()
  set(BENCH_ARCH_FLAG -march=x86-64-v3)
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} ${BENCH_ARCH_FLAG} -fno-omit-frame-pointer" BENCH_COMPILER_FLAGS)

# ---- warnings / sanitizers shared by all our targets -----------------------------------
add_library(bench_options INTERFACE)
target_compile_options(bench_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror
  ${BENCH_ARCH_FLAG} -fno-omit-frame-pointer)
if(BENCH_SANITIZER STREQUAL "address")
  target_compile_options(bench_options INTERFACE -fsanitize=address,undefined)
  target_link_options(bench_options INTERFACE -fsanitize=address,undefined)
elseif(BENCH_SANITIZER STREQUAL "thread")
  target_compile_options(bench_options INTERFACE -fsanitize=thread)
  target_link_options(bench_options INTERFACE -fsanitize=thread)
elseif(NOT BENCH_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown BENCH_SANITIZER='${BENCH_SANITIZER}'")
endif()

# ---- core library ----------------------------------------------------------------------
add_library(bench_core STATIC
  src/metric.cpp
  src/sha256.cpp
  src/sysinfo.cpp
  src/result.cpp
  src/validate.cpp
  src/runner.cpp
  src/workloads/cpu_int.cpp)
target_include_directories(bench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bench_core PUBLIC nlohmann_json::nlohmann_json bench_options)
target_compile_definitions(bench_core PRIVATE
  BENCH_VERSION="${PROJECT_VERSION}"
  BENCH_GIT_SHA="${BENCH_GIT_SHA}"
  BENCH_COMPILER_FLAGS="${BENCH_COMPILER_FLAGS}")
find_package(Threads REQUIRED)
target_link_libraries(bench_core PUBLIC Threads::Threads)

# ---- CLI -------------------------------------------------------------------------------
add_executable(bench src/main.cpp)
target_link_libraries(bench PRIVATE bench_core cli11_system)

# ---- tests -----------------------------------------------------------------------------
if(BENCH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
