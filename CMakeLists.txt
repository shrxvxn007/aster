# Aster — INTERFACE library for headers, OBJECT library for .cpp files.

add_library(aster INTERFACE)
target_include_directories(aster INTERFACE include)
target_compile_features(aster INTERFACE cxx_std_20)

# Platform flags
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  set(ARCH_FLAG "-mcpu=apple-m1")
else()
  set(ARCH_FLAG "-march=native")
endif()

# OBJECT library for shared implementation (no main() functions)
add_library(aster_obj OBJECT
  src/replay/parser.cpp
  src/replay/replay_engine.cpp
  src/strategy/analytics.cpp
  src/strategy/mm_strategy.cpp
  src/strategy/backtest.cpp
)
target_include_directories(aster_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(aster_obj PUBLIC cxx_std_20)

target_compile_options(aster_obj PRIVATE
  $<$<CONFIG:Release>:-O3 ${ARCH_FLAG} -flto -DNDEBUG>
  $<$<CONFIG:Debug>:-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer>
)

target_link_options(aster_obj PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

target_link_libraries(aster_obj PUBLIC aster)

# Executables — each has its own main.cpp, links against aster_obj.
add_executable(aster_replay src/main.cpp)
target_link_libraries(aster_replay PRIVATE aster_obj aster)
target_link_options(aster_replay PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_executable(aster_sim src/sim.cpp)
target_link_libraries(aster_sim PRIVATE aster_obj aster)
target_link_options(aster_sim PRIVATE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
