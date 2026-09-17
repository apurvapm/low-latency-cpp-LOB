cmake_minimum_required(VERSION 3.20)
project(lob_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(BUILD_BENCHMARKS "Build the Google Benchmark harness" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lob_core STATIC
  src/matching_engine.cpp
  src/trading_bot.cpp
)
target_include_directories(lob_core PUBLIC include)
target_compile_features(lob_core PUBLIC cxx_std_20)
target_compile_options(lob_core PUBLIC -Wall -Wextra -Wpedantic)
# Statically link the runtime so binaries don't depend on whichever
# libstdc++-6.dll happens to be first on PATH (on this MinGW/MSYS2 setup,
# Git's bundled mingw64 copy shadows the toolchain's own and is ABI-
# incompatible, causing runtime crashes). Harmless on Linux too.
target_link_options(lob_core PUBLIC -static-libgcc -static-libstdc++)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT (ENABLE_ASAN OR ENABLE_TSAN))
  target_compile_options(lob_core PUBLIC -O3 -march=native)
endif()

if(ENABLE_ASAN)
  target_compile_options(lob_core PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(lob_core PUBLIC -fsanitize=address)
endif()

if(ENABLE_UBSAN)
  target_compile_options(lob_core PUBLIC -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  target_link_options(lob_core PUBLIC -fsanitize=undefined -fno-sanitize-recover=all)
endif()

if(ENABLE_TSAN)
  target_compile_options(lob_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(lob_core PUBLIC -fsanitize=thread)
endif()

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob_core)

add_executable(lob_cli cli/lob_cli.cpp)
target_link_libraries(lob_cli PRIVATE lob_core)

add_executable(lob_latency bench/latency_mixed.cpp)
target_link_libraries(lob_latency PRIVATE lob_core)

enable_testing()
add_executable(lob_tests tests/test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob_core)
add_test(NAME lob_tests COMMAND lob_tests)

# Overrides the global allocator to count hot-path allocations, which fights
# a sanitizer's own allocator, so this only builds in a plain build.
if(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN)
  add_executable(lob_test_no_alloc tests/test_no_alloc.cpp)
  target_link_libraries(lob_test_no_alloc PRIVATE lob_core)
  add_test(NAME lob_test_no_alloc COMMAND lob_test_no_alloc)
endif()

add_executable(lob_test_flat_id_map tests/test_flat_id_map.cpp)
target_link_libraries(lob_test_flat_id_map PRIVATE lob_core)
add_test(NAME lob_test_flat_id_map COMMAND lob_test_flat_id_map)

add_executable(lob_test_level_bitmap tests/test_level_bitmap.cpp)
target_link_libraries(lob_test_level_bitmap PRIVATE lob_core)
add_test(NAME lob_test_level_bitmap COMMAND lob_test_level_bitmap)

if(BUILD_BENCHMARKS)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  # Upstream sysinfo.cc has a non-exhaustive switch on newer GCC targeting
  # Windows; not our code to fix.
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(lob_bench bench/bench_matching_engine.cpp)
  target_link_libraries(lob_bench PRIVATE lob_core benchmark::benchmark)
endif()
