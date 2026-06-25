cmake_minimum_required(VERSION 3.16)
project(hybrid_memory_btrees LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra)

add_executable(benchmark
  src/main.cpp
)
target_include_directories(benchmark PRIVATE include)

add_executable(correctness
  tests/correctness.cpp
)
target_include_directories(correctness PRIVATE include)

enable_testing()
add_test(NAME correctness COMMAND correctness)
