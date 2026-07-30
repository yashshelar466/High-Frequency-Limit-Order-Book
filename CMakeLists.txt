cmake_minimum_required(VERSION 3.14)
project(HighFrequencyLimitOrderBook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build so out-of-the-box numbers are representative.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    # Applied before any target is defined so every target inherits it.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Core matching engine as a small static library shared by every target.
add_library(orderbook STATIC src/OrderBook.cpp)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- Executables -----------------------------------------------------------

add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE orderbook)

add_executable(run_feed src/replay_main.cpp)
target_link_libraries(run_feed PRIVATE orderbook)

add_executable(run_tests tests/test_matching.cpp)
target_link_libraries(run_tests PRIVATE orderbook)

add_executable(run_diff tests/test_differential.cpp)
target_link_libraries(run_diff PRIVATE orderbook)


add_executable(run_pool tests/test_memorypool.cpp)
target_link_libraries(run_pool PRIVATE orderbook)

# LOBSTER market-data replay: reconciles the reconstructed book against a
# venue-published book. The CLI takes real data files (not redistributed here);
# the test drives the same logic with synthetic LOBSTER-format fixtures.
add_executable(run_lobster src/lobster_replay.cpp)
target_link_libraries(run_lobster PRIVATE orderbook)

add_executable(run_lobster_test tests/test_lobster_replay.cpp)
target_link_libraries(run_lobster_test PRIVATE orderbook)

# The latency benchmark always wants aggressive optimization, independent of
# the overall build type.
add_executable(run_benchmark benchmarks/benchmark_latency.cpp)
target_link_libraries(run_benchmark PRIVATE orderbook)
target_compile_options(run_benchmark PRIVATE -O3)

# --- Tests -----------------------------------------------------------------

enable_testing()
add_test(NAME unit COMMAND run_tests)
add_test(NAME differential COMMAND run_diff)

add_test(NAME memory_pool COMMAND run_pool)

add_test(NAME lobster_replay COMMAND run_lobster_test)
