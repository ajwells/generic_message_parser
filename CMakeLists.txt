cmake_minimum_required(VERSION 3.25)
project(generic_message_parser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GMP_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
option(GMP_BUILD_GENERATOR "Build the gmpc code generator" ON)
option(GMP_BUILD_TESTS "Build GMP tests" ${GMP_IS_TOP_LEVEL})
option(GMP_BUILD_EXAMPLES "Build GMP examples" ${GMP_IS_TOP_LEVEL})
option(GMP_BUILD_BENCH "Build GMP benchmarks" OFF)

include(FetchContent)

# ---------------------------------------------------------------------------
# Runtime: header-only, standard library only, embedded safe.
# ---------------------------------------------------------------------------
add_library(gmp_runtime INTERFACE)
add_library(gmp::runtime ALIAS gmp_runtime)
target_include_directories(gmp_runtime INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>)
target_compile_features(gmp_runtime INTERFACE cxx_std_23)

# ---------------------------------------------------------------------------
# Generator
# ---------------------------------------------------------------------------
if (GMP_BUILD_GENERATOR)
    add_subdirectory(generator)
endif ()

include(cmake/GmpGenerate.cmake)

if (GMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (GMP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (GMP_BUILD_BENCH)
    add_subdirectory(bench)
endif ()
