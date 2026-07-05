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
# Debug  : -O0, ASan + UBSan (local-only; CI runs Release only because the
# instrumented -O0 binary is 30-50x slower and times out ctest on macOS).
target_compile_options(aster_obj PRIVATE
  $<$<CONFIG:Release>:-O3 ${ARCH_FLAG} -flto -DNDEBUG>
  $<$<CONFIG:Debug>:-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(aster_obj PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
target_link_libraries(aster_obj PUBLIC aster)

# Backtest CLI: replays ITCH and runs the Avellaneda-Stoikov market maker.
add_executable(aster_replay src/main.cpp)
target_link_libraries(aster_replay PRIVATE aster_obj aster)
target_link_options(aster_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# Synthetic benchmark: Poisson arrival process, no ITCH file required.
add_executable(aster_sim src/sim.cpp)
target_link_libraries(aster_sim PRIVATE aster_obj aster)
target_link_options(aster_sim PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE aster_obj aster)
target_compile_options(test_engine PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
target_link_options(test_engine PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE aster_obj aster)
target_compile_options(test_parser PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
target_link_options(test_parser PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
add_test(NAME test_parser COMMAND test_parser)

add_executable(test_replay tests/test_replay.cpp)
target_link_libraries(test_replay PRIVATE aster_obj aster)
target_compile_options(test_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
target_link_options(test_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
add_test(NAME test_replay COMMAND test_replay)
