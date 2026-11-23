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

# Python 3.12+ for the Python tests: GMP_PYTHON, PATH, or a portable / emsdk install in the home directory.
file(GLOB _gmp_python_dirs "$ENV{USERPROFILE}/tools/python3*" "$ENV{HOME}/tools/python3*" "$ENV{EMSDK}/python/*"
        "$ENV{USERPROFILE}/emsdk/python/*" "$ENV{HOME}/emsdk/python/*")
if (NOT GMP_PYTHON)
    foreach (_cand IN ITEMS python3 python)
        find_program(_gmp_py_${_cand} NAMES ${_cand})
        list(APPEND _gmp_py_candidates ${_gmp_py_${_cand}})
    endforeach ()
    foreach (_dir IN LISTS _gmp_python_dirs)
        list(APPEND _gmp_py_candidates "${_dir}/python.exe" "${_dir}/bin/python3")
    endforeach ()
    foreach (_cand IN LISTS _gmp_py_candidates)
        if (EXISTS "${_cand}")
            execute_process(COMMAND "${_cand}" -c "import sys; print(sys.version_info >= (3, 12))"
                    OUTPUT_VARIABLE _ok OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if (_ok STREQUAL "True")
                set(GMP_PYTHON "${_cand}" CACHE FILEPATH "Python 3.12+ for the Python tests")
                break()
            endif ()
        endif ()
    endforeach ()
endif ()

add_subdirectory(runtime)
add_subdirectory(runtime_c)
add_subdirectory(runtime_js)
add_subdirectory(runtime_py)
if (GMP_BUILD_GENERATOR)
    add_subdirectory(generator)
    add_subdirectory(roundtrip)
    add_subdirectory(roundtrip_c)
    add_subdirectory(roundtrip_js)
    add_subdirectory(roundtrip_py)
endif ()
