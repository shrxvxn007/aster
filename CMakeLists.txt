# Aster — C++20 low-latency matching engine & market-making simulator.
#
# Layout: INTERFACE library for headers, OBJECT library for shared .cpp
# (instantiated once, no duplicated template expansions). Two executables:
#   aster_replay — ITCH replay + market-making backtest CLI
#   aster_sim    — synthetic engine benchmark (Poisson arrival process)

cmake_minimum_required(VERSION 3.16)
project(aster LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-specific optimization.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  set(ARCH_FLAG "-mcpu=apple-m1")
else()
  set(ARCH_FLAG "-march=native")
endif()

# INTERFACE library (header-only API surface).
add_library(aster INTERFACE)
target_include_directories(aster INTERFACE include)
target_compile_features(aster INTERFACE cxx_std_20)

# OBJECT library: shared implementation, compiled once across all binaries.
add_library(aster_obj OBJECT
  src/core/types.cpp
  src/replay/parser.cpp
  src/replay/replay_engine.cpp
  src/strategy/analytics.cpp
  src/strategy/mm_strategy.cpp
  src/strategy/risk_manager.cpp
  src/strategy/backtest.cpp
)
target_include_directories(aster_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aster_obj PUBLIC cxx_std_20)

# Release: -O3, LTO, arch-tuned, NDEBUG.
# Debug  : -O1 + UBSan + frame pointers. ASan was tried first but had two
# distinct failure modes in CI: (1) at -O0 it inflated ctest per-event
# wallclock 30-50x and timed out the 120-s cap; (2) at -O1 it still
# produced instrumented binaries large enough that cold-ccache linking
# flaked on the GH Actions runner ('Build' step failures). UBSan without
# ASan simplifies the linker footprint (no ASan runtime, no shadow-memory
# metadata), keeps per-test wallclock under 5 s on Linux gcc, AND
# surfaces the runtime-error UB signal the user actually wants
# (signed integer overflow, null pointer, alignment, invalid shift).
# -O1 still triggers UBSan checks (sanitizers don't require -O0).
# -fno-omit-frame-pointer produces accurate stack traces.
# NB: -O1 may mask some UB paths that -O0 surfaces (compiler inlining /
# constant propagation elides UB-free subpaths), so this is a practical
# tradeoff: the CI Debug matrix entry is a fast UB-regression detector,
# not a UB oracle. Before re-enabling ASan, audit the binary size + the
# link-step memory ceiling on the GH Actions runner.
target_compile_options(aster_obj PRIVATE
  $<$<CONFIG:Release>:-O3 ${ARCH_FLAG} -flto -DNDEBUG>
  $<$<CONFIG:Debug>:-O1 -g -fsanitize=undefined -fno-omit-frame-pointer>
)
target_link_options(aster_obj PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)
target_link_libraries(aster_obj PUBLIC aster)

# Backtest CLI: replays ITCH and runs the Avellaneda-Stoikov market maker.
add_executable(aster_replay src/main.cpp)
target_link_libraries(aster_replay PRIVATE aster_obj aster)
target_link_options(aster_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)

# Synthetic benchmark: Poisson arrival process, no ITCH file required.
add_executable(aster_sim src/sim.cpp)
target_link_libraries(aster_sim PRIVATE aster_obj aster)
target_link_options(aster_sim PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)

# -----------------------------------------------------------------------------
# Synthetic-benchmark throughput-floor target.
#
# Wraps a Release-mode `aster_sim --events N` invocation through
# scripts/perf_floor_check.py, which parses the `Throughput: <float> M
# events/sec` line and asserts the measured rate is >= PERF_FLOOR_M_EV_PER_SEC.
# Mirrors the inline awk block in .github/workflows/ci.yml::perf-floor so
# the floor logic lives in exactly one place and local `cmake --build
# build --target perf-floor` produces the same gate decision as the CI job.
#
# Configure-time overrides:
#   -DPERF_FLOOR_M_EV_PER_SEC=12.0   raise the floor
#   -DPERF_FLOOR_EVENTS=2000000      raise the event count
# Build-time override (per-invocation):
#   cmake --build build --target perf-floor -DPERF_FLOOR_EVENTS=500000
# -----------------------------------------------------------------------------
if(NOT DEFINED PERF_FLOOR_M_EV_PER_SEC)
  set(PERF_FLOOR_M_EV_PER_SEC 10.0 CACHE FLOAT
      "Throughput floor for `perf-floor` target + CI perf-floor job (M events/sec)")
endif()
if(NOT DEFINED PERF_FLOOR_EVENTS)
  set(PERF_FLOOR_EVENTS 1000000 CACHE INTEGER
      "Event count for `perf-floor` target + CI perf-floor job")
endif()

add_custom_target(perf-floor
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/perf_floor_check.py"
          --binary "$<TARGET_FILE:aster_sim>"
          --events "${PERF_FLOOR_EVENTS}"
          --floor "${PERF_FLOOR_M_EV_PER_SEC}"
  DEPENDS aster_sim
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Running synthetic-benchmark throughput-floor check (floor=${PERF_FLOOR_M_EV_PER_SEC} M events/sec, events=${PERF_FLOOR_EVENTS})"
  VERBATIM
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE aster_obj aster)
target_compile_options(test_engine PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined -fno-omit-frame-pointer>
)
target_link_options(test_engine PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE aster_obj aster)
target_compile_options(test_parser PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined -fno-omit-frame-pointer>
)
target_link_options(test_parser PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_test(NAME test_parser COMMAND test_parser)

add_executable(test_replay tests/test_replay.cpp)
target_link_libraries(test_replay PRIVATE aster_obj aster)
target_compile_options(test_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined -fno-omit-frame-pointer>
)
target_link_options(test_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_test(NAME test_replay COMMAND test_replay)

# -----------------------------------------------------------------------------
# Python regression tests
# -----------------------------------------------------------------------------
# CLI flag-parser regression gate. Asserts every --flag passed to
# aster_replay/aster_sim in any .sh/.py/.yml callsite is registered in
# the relevant binary's std::strcmp parser. Stdlib Python 3 only; no
# pytest dependency. Wired into the CI build job's ctest matrix so flag
# drift fails at PR time with a file:line reference, not silently at the
# next CI run of an offline workflow.
add_test(NAME test_cli_flag_parser
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_flag_parser.py
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})# -----------------------------------------------------------------------------
# CI snapshot regression gate
# -----------------------------------------------------------------------------
# Asserts that the offline ci/ mirror captured by scripts/capture_ci.sh
# represents a SETTLED green signal: every job in
# ci/last_run_jobs.json has conclusion=="success" AND badge.svg's first
# <title> matches "CI - passing". Exits 2 on missing/malformed files, 1
# on red matrix or stale badge, 0 on green.
#
# Registered UNCONDITIONALLY (no `if(EXISTS ci/...)` guard) because
# ci/last_run_jobs.json and ci/badge.svg are committed to the repo, so
# the prior guard evaluated to true on every fresh CI clone and the
# test got pulled into the default `ctest --output-on-failure` run in
# `.github/workflows/ci.yml`. That created a chicken-and-egg deadlock:
#   * the test asserts the mirror is green,
#   * the mirror only updates when capture-ci.yml cron runs and
#     captures the latest CI matrix state,
#   * if the matrix itself was red, the mirror committed the red state
#     and the test failed for the next run -> permanent red.
#
# CMake does not expose a per-test `EXCLUDE_FROM_ALL` (that flag only
# affects build targets like `add_executable`), so the right gate lives
# at the CTest invocation site:
#   * `.github/workflows/ci.yml` excludes it by name (`-E test_ci_snapshot`)
#     so the matrix never blocks on its own offline-mirror state. We use
#     `-E <name>` (CMake 3.0+) rather than `--label-exclude` (3.20+) for
#     compatibility with the older Apple CMake on macos-latest runners.
#   * Local developer: `ctest -L ci` runs it on demand.
#   * If ci/ artifacts are genuinely absent (e.g. a pre-capture clone),
#     the test exits 2 with a clear error - no CMake-time gating needed.
# -----------------------------------------------------------------------------
add_test(NAME test_ci_snapshot
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ci_snapshot.py
                 --repo-root ${CMAKE_CURRENT_SOURCE_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_ci_snapshot PROPERTIES LABELS "ci")
