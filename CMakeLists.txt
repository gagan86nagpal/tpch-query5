cmake_minimum_required(VERSION 3.10)
project(tpch_query5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to a Release build for meaningful benchmark numbers.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(tpch_query5 src/main.cpp src/query5.cpp)

target_include_directories(tpch_query5 PRIVATE include)

target_compile_options(tpch_query5 PRIVATE
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    -Wall -Wextra
)

find_package(Threads REQUIRED)
target_link_libraries(tpch_query5 PRIVATE Threads::Threads)
