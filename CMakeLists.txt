cmake_minimum_required(VERSION 3.20)
project(Aster LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(aster
    src/main.cpp
    src/order_book.cpp
    src/matching_engine.cpp
    src/itch_parser.cpp
    src/replay_driver.cpp
    src/latency_profiler.cpp
    src/market_maker.cpp
)
target_include_directories(aster PRIVATE include)
target_link_libraries(aster PRIVATE pthread)

add_executable(generate_itch tools/generate_itch.cpp)
target_include_directories(generate_itch PRIVATE include)

add_executable(multithreaded_aster
    tools/multithreaded_example.cpp
    src/order_book.cpp
    src/matching_engine.cpp
    src/itch_parser.cpp
    src/replay_driver.cpp
    src/latency_profiler.cpp
)
target_include_directories(multithreaded_aster PRIVATE include)
target_link_libraries(multithreaded_aster PRIVATE pthread)

add_executable(run_tests
    tests/test_matching.cpp
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(run_tests PRIVATE include)
