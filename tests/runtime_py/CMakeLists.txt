# Python runtime tests (runtime-py/gmsg) with Python 3.12+: unit checks plus a cross-check against the C++ runtime
# (the language-independent reference vectors of gmp_js_vectors). Without a suitable Python the tests are not
# registered.
#
# Every Python test runs twice: pure Python (--pure), and with the accelerator gmsg._native (--native) when it can be
# built: the C compiler plus the Python headers (and on Windows libs/python3.lib) of GMP_PYTHON. The accelerator is
# built here against the limited API (abi3, 3.12) and placed with a copy of the package in <build>/py/gmsg.

if (NOT GMP_PYTHON)
    message(STATUS "GMP: Python 3.12+ not found, Python tests are disabled")
    return()
endif ()

add_test(NAME gmp_py_runtime_tests
        COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime.py $<TARGET_FILE:gmp_js_vectors> --pure)
set_tests_properties(gmp_py_runtime_tests PROPERTIES ENVIRONMENT "GMSG_PATH=${PROJECT_SOURCE_DIR}/runtime-py")

# ---- the accelerator ------------------------------------------------------------------------------------------------
set(GMP_PY_NATIVE_DIR "" CACHE INTERNAL "package directory with the built accelerator (empty: not built)")
get_property(_gmp_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if (NOT "C" IN_LIST _gmp_languages)
    return()
endif ()
execute_process(COMMAND ${GMP_PYTHON} -c
        "import os, sys, sysconfig; print(sysconfig.get_paths()['include']); print(os.path.join(sys.base_prefix, 'libs', 'python3.lib'))"
        OUTPUT_VARIABLE _py_paths OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REPLACE "\n" ";" _py_paths "${_py_paths}")
list(GET _py_paths 0 _py_include)
list(GET _py_paths 1 _py_lib)
if (NOT EXISTS "${_py_include}/Python.h" OR (WIN32 AND NOT EXISTS "${_py_lib}"))
    message(STATUS "GMP: no Python headers for ${GMP_PYTHON}, the gmsg accelerator is not built (pure-Python tests only)")
    return()
endif ()

set(_pkg ${CMAKE_BINARY_DIR}/py/gmsg)
add_library(gmsg_native MODULE ${PROJECT_SOURCE_DIR}/runtime-py/native/_native.c)
target_include_directories(gmsg_native PRIVATE "${_py_include}")
target_link_libraries(gmsg_native PRIVATE gmp::runtime_c)
target_compile_definitions(gmsg_native PRIVATE Py_LIMITED_API=0x030C0000)
set_target_properties(gmsg_native PROPERTIES OUTPUT_NAME _native PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${_pkg})
if (WIN32)
    target_link_libraries(gmsg_native PRIVATE "${_py_lib}")
    target_link_options(gmsg_native PRIVATE $<$<C_COMPILER_ID:GNU>:-static-libgcc>)
    set_target_properties(gmsg_native PROPERTIES SUFFIX ".pyd")
else ()
    set_target_properties(gmsg_native PROPERTIES SUFFIX ".abi3.so")
    if (APPLE)
        target_link_options(gmsg_native PRIVATE -undefined dynamic_lookup)
    endif ()
endif ()
# the package sources next to the module (copied on every build, so edits of runtime-py/gmsg are picked up)
add_custom_target(gmsg_py_package ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${PROJECT_SOURCE_DIR}/runtime-py/gmsg ${_pkg}
        COMMENT "gmp: copying the Python runtime next to the accelerator")
add_dependencies(gmsg_native gmsg_py_package)
set(GMP_PY_NATIVE_DIR ${CMAKE_BINARY_DIR}/py CACHE INTERNAL "")

add_test(NAME gmp_py_runtime_tests_native
        COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime.py $<TARGET_FILE:gmp_js_vectors> --native)
set_tests_properties(gmp_py_runtime_tests_native PROPERTIES ENVIRONMENT "GMSG_PATH=${GMP_PY_NATIVE_DIR}")
