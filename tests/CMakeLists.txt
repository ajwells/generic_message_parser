FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

if (MINGW)
    # Keep test binaries self-contained (no dependency on MinGW DLLs being on PATH).
    set(GMP_STATIC_LINK -static-libgcc -static-libstdc++)
endif ()

# gmp_add_test(<name> <sources>... [TEST_SUFFIX <text>]): a Catch2 executable; TEST_SUFFIX is appended to the
# ctest names (needed when the same test sources are built twice).
function(gmp_add_test name)
    cmake_parse_arguments(T "" "TEST_SUFFIX" "" ${ARGN})
    add_executable(${name} ${T_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain gmp::runtime)
    target_compile_options(${name} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)
    target_link_options(${name} PRIVATE ${GMP_STATIC_LINK})
    if (T_TEST_SUFFIX)
        catch_discover_tests(${name} TEST_SUFFIX "${T_TEST_SUFFIX}")
    else ()
        catch_discover_tests(${name})
    endif ()
endfunction()

# Strict C warnings for the C runtime and generated C code (warnings are errors).
set(GMP_C_WARNINGS $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        -Wstrict-prototypes -Wmissing-prototypes -Werror>)

# Node.js for the JavaScript tests: PATH, or the copy that comes with emsdk (EMSDK, or ~/emsdk).
file(GLOB _gmp_emsdk_node_dirs "$ENV{EMSDK}/node/*" "$ENV{USERPROFILE}/emsdk/node/*" "$ENV{HOME}/emsdk/node/*")
find_program(GMP_NODE NAMES node PATHS ${_gmp_emsdk_node_dirs} PATH_SUFFIXES bin)

add_subdirectory(runtime)
add_subdirectory(runtime_c)
add_subdirectory(runtime_js)
if (GMP_BUILD_GENERATOR)
    add_subdirectory(generator)
    add_subdirectory(roundtrip)
    add_subdirectory(roundtrip_c)
    add_subdirectory(roundtrip_js)
endif ()
