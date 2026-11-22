# Tests of the C runtime (runtime-c/). Warnings are errors: generated C code includes these headers.
# GMP_C_WARNINGS (strict C11 warnings, -Werror) is defined in tests/CMakeLists.txt.

# Pure C unit tests: C11 and C99, with the C library and without (GMSG_USE_LIBC=0), header-only.
foreach (std IN ITEMS 11 99)
    foreach (libc IN ITEMS 1 0)
        if (std STREQUAL "11")
            set(name gmsg_c_runtime_tests_libc${libc})
        else ()
            set(name gmsg_c_runtime_tests_c99_libc${libc})
        endif ()
        add_executable(${name} test_c_runtime.c)
        set_target_properties(${name} PROPERTIES C_STANDARD ${std} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_definitions(${name} PRIVATE GMSG_USE_LIBC=${libc})
        target_compile_options(${name} PRIVATE ${GMP_C_WARNINGS})
        target_link_libraries(${name} PRIVATE gmp::runtime_c)
        target_link_options(${name} PRIVATE ${GMP_STATIC_LINK})
        add_test(NAME ${name} COMMAND ${name})
    endforeach ()
endforeach ()

# Header-only with the CRC lookup tables (GMSG_CRC_TABLES=1).
add_executable(gmsg_c_runtime_tests_crc_tables test_c_runtime.c)
set_target_properties(gmsg_c_runtime_tests_crc_tables PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(gmsg_c_runtime_tests_crc_tables PRIVATE GMSG_CRC_TABLES=1)
target_compile_options(gmsg_c_runtime_tests_crc_tables PRIVATE ${GMP_C_WARNINGS})
target_link_libraries(gmsg_c_runtime_tests_crc_tables PRIVATE gmp::runtime_c)
target_link_options(gmsg_c_runtime_tests_crc_tables PRIVATE ${GMP_STATIC_LINK})
add_test(NAME gmsg_c_runtime_tests_crc_tables COMMAND gmsg_c_runtime_tests_crc_tables)

# The compiled library build (gmp::runtime_c_lib: GMSG_RUNTIME_LIBRARY=1, CRC tables), itself built as C99 with
# the strict warnings.
target_compile_options(gmp_runtime_c_lib PRIVATE ${GMP_C_WARNINGS})
add_executable(gmsg_c_runtime_tests_lib test_c_runtime.c)
set_target_properties(gmsg_c_runtime_tests_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(gmsg_c_runtime_tests_lib PRIVATE ${GMP_C_WARNINGS})
target_link_libraries(gmsg_c_runtime_tests_lib PRIVATE gmp::runtime_c_lib)
target_link_options(gmsg_c_runtime_tests_lib PRIVATE ${GMP_STATIC_LINK})
add_test(NAME gmsg_c_runtime_tests_lib COMMAND gmsg_c_runtime_tests_lib)

# The C library float text backend (GMSG_FLOAT_TEXT_LIBC: snprintf / strtod).
add_executable(gmsg_c_runtime_tests_float_libc test_c_runtime.c)
set_target_properties(gmsg_c_runtime_tests_float_libc PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(gmsg_c_runtime_tests_float_libc PRIVATE GMSG_FLOAT_TEXT=GMSG_FLOAT_TEXT_LIBC)
target_compile_options(gmsg_c_runtime_tests_float_libc PRIVATE ${GMP_C_WARNINGS})
target_link_libraries(gmsg_c_runtime_tests_float_libc PRIVATE gmp::runtime_c)
target_link_options(gmsg_c_runtime_tests_float_libc PRIVATE ${GMP_STATIC_LINK})
add_test(NAME gmsg_c_runtime_tests_float_libc COMMAND gmsg_c_runtime_tests_float_libc)

# C/C++ cross-check: the C side is compiled as C11, the comparison is a Catch2 test against the C++ runtime.
add_library(gmp_c_crosscheck_c STATIC crosscheck_c.c crosscheck_framing_c.c crosscheck_text_c.c)
set_target_properties(gmp_c_crosscheck_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(gmp_c_crosscheck_c PRIVATE ${GMP_C_WARNINGS})
target_link_libraries(gmp_c_crosscheck_c PUBLIC gmp::runtime_c)
target_include_directories(gmp_c_crosscheck_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

gmp_add_test(gmp_c_vs_cpp_tests test_c_vs_cpp.cpp test_framing_c_vs_cpp.cpp test_text_c_vs_cpp.cpp cxx_compat.cpp)
target_link_libraries(gmp_c_vs_cpp_tests PRIVATE gmp_c_crosscheck_c)

# The same cross-check against the library build (C side compiled as C99, CRC tables).
add_library(gmp_c_crosscheck_c_lib STATIC crosscheck_c.c crosscheck_framing_c.c crosscheck_text_c.c)
set_target_properties(gmp_c_crosscheck_c_lib PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(gmp_c_crosscheck_c_lib PRIVATE ${GMP_C_WARNINGS})
target_link_libraries(gmp_c_crosscheck_c_lib PUBLIC gmp::runtime_c_lib)
target_include_directories(gmp_c_crosscheck_c_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
gmp_add_test(gmp_c_vs_cpp_tests_lib test_c_vs_cpp.cpp test_framing_c_vs_cpp.cpp test_text_c_vs_cpp.cpp cxx_compat.cpp
        TEST_SUFFIX " [c99, runtime_c_lib]")
target_link_libraries(gmp_c_vs_cpp_tests_lib PRIVATE gmp_c_crosscheck_c_lib)
