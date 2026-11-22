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

if (GMP_BUILD_TESTS OR GMP_BUILD_EXAMPLES)
    enable_language(C)  # C runtime tests, C examples
endif ()

# C runtime (C99 or later, optional C library use; see runtime-c/include/gmp/c/config.h):
#   gmp::runtime_c      header-only
#   gmp::runtime_c_lib  compiled library (GMSG_RUNTIME_LIBRARY=1, CRC lookup tables); defined when the C language
#                       is enabled. Set runtime options on it with PUBLIC definitions so its users see the same
#                       values, e.g. target_compile_definitions(gmp_runtime_c_lib PUBLIC GMSG_USE_LIBC=0).
# Consumers enable the C language themselves; the repository does so only for its tests and examples.
add_library(gmp_runtime_c INTERFACE)
add_library(gmp::runtime_c ALIAS gmp_runtime_c)
target_include_directories(gmp_runtime_c INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime-c/include>)

get_property(_gmp_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if ("C" IN_LIST _gmp_languages)
    add_library(gmp_runtime_c_lib STATIC
            runtime-c/src/checksum.c
            runtime-c/src/framing.c
            runtime-c/src/stream.c
            runtime-c/src/float_text.c
            runtime-c/src/text.c
            runtime-c/src/print.c)
    add_library(gmp::runtime_c_lib ALIAS gmp_runtime_c_lib)
    target_link_libraries(gmp_runtime_c_lib PUBLIC gmp_runtime_c)
    target_compile_definitions(gmp_runtime_c_lib PUBLIC GMSG_RUNTIME_LIBRARY=1)
    set_target_properties(gmp_runtime_c_lib PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    # one section per function and table, so a linker with --gc-sections drops what is not used
    target_compile_options(gmp_runtime_c_lib PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-ffunction-sections -fdata-sections>)
endif ()

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
