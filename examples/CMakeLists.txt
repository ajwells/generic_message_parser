# Examples: every program exists in C++ (main*.cpp), C (main*.c), JavaScript (main*.js) and Python (main*.py), and
# the four print the same text. With tests enabled, each run is compared with the example's expected_output*.txt
# (ctest -R example), so the languages are checked against each other.
#
# gmp_add_example(<name> SCHEMAS <yaml>... SOURCES <files>... [LANG cpp|c|js|python] [FILES <files>...]
#                 [EXPECTED <file>])
#   cpp / c: an executable. C examples are C11 programs using the C backend and runtime-c.
#   js / python: the generated modules and SOURCES + FILES (e.g. codec modules) are put in one directory, where the
#   first SOURCES file is run with Node.js (GMP_NODE) / Python 3.12+ (GMP_PYTHON; runtime-py on PYTHONPATH).
#   EXPECTED: the output the program must print (compared without \r, since C stdio on Windows writes \r\n).
set(GMP_EXAMPLE_COMPARE ${CMAKE_CURRENT_SOURCE_DIR}/compare_output.cmake)

function(_gmp_example_test name lang expected)
    if (NOT expected OR NOT TARGET ${name}_msgs)
        return()
    endif ()
    cmake_parse_arguments(T "" "WORKDIR" "COMMAND;ENV" ${ARGN})
    string(REGEX REPLACE "^example_" "" label "${name}")
    string(REGEX REPLACE "_(c|js|py)$" "" label "${label}")
    add_test(NAME "example ${label} [${lang}] output"
            COMMAND ${CMAKE_COMMAND} "-DEXPECTED=${expected}" "-DWORKDIR=${T_WORKDIR}" "-DPROGRAM=${T_COMMAND}"
            -P ${GMP_EXAMPLE_COMPARE})
    if (T_ENV)
        set_tests_properties("example ${label} [${lang}] output" PROPERTIES ENVIRONMENT "${T_ENV}")
    endif ()
endfunction()

function(gmp_add_example name)
    cmake_parse_arguments(EX "" "LANG;EXPECTED" "SCHEMAS;SOURCES;FILES" ${ARGN})
    if (NOT EX_LANG)
        set(EX_LANG cpp)
    endif ()
    if (EX_EXPECTED)
        get_filename_component(EX_EXPECTED "${EX_EXPECTED}" ABSOLUTE)
    endif ()

    if (EX_LANG STREQUAL "js" OR EX_LANG STREQUAL "python")
        if (EX_LANG STREQUAL "js" AND NOT GMP_NODE)
            return()
        endif ()
        if (EX_LANG STREQUAL "python" AND NOT GMP_PYTHON)
            return()
        endif ()
        set(dir ${CMAKE_CURRENT_BINARY_DIR}/${name})
        gmp_generate(TARGET ${name}_msgs LANG ${EX_LANG} OUT_DIR ${dir} SCHEMAS ${EX_SCHEMAS})
        set(copied "")
        foreach (f IN LISTS EX_SOURCES EX_FILES)
            get_filename_component(fname "${f}" NAME)
            add_custom_command(OUTPUT ${dir}/${fname}
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${f} ${dir}/${fname}
                    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${f} VERBATIM)
            list(APPEND copied ${dir}/${fname})
        endforeach ()
        add_custom_target(${name} ALL DEPENDS ${copied})
        add_dependencies(${name} ${name}_msgs)
        list(GET EX_SOURCES 0 main)
        get_filename_component(main "${main}" NAME)
        if (EX_LANG STREQUAL "js")
            _gmp_example_test(${name} js "${EX_EXPECTED}" WORKDIR ${dir} COMMAND "${GMP_NODE};${main}")
        else ()
            _gmp_example_test(${name} python "${EX_EXPECTED}" WORKDIR ${dir} COMMAND "${GMP_PYTHON};${main}"
                    ENV "PYTHONPATH=${PROJECT_SOURCE_DIR}/runtime-py;GMSG_PURE=1")
            if (GMP_PY_NATIVE_DIR)  # the same program with the compiled accelerator
                _gmp_example_test(${name} "python native" "${EX_EXPECTED}" WORKDIR ${dir} COMMAND "${GMP_PYTHON};${main}"
                        ENV "PYTHONPATH=${GMP_PY_NATIVE_DIR}")
            endif ()
        endif ()
        return()
    endif ()

    gmp_generate(TARGET ${name}_msgs LANG ${EX_LANG} SCHEMAS ${EX_SCHEMAS})
    add_executable(${name} ${EX_SOURCES})
    target_link_libraries(${name} PRIVATE ${name}_msgs)
    if (EX_LANG STREQUAL "c")
        # the generated .gmp.c is compiled too, and may include user codec headers from this directory
        target_include_directories(${name}_msgs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        set_target_properties(${name} ${name}_msgs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    else ()
        target_include_directories(${name}_msgs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif ()
    if (MINGW)
        target_link_options(${name} PRIVATE -static-libgcc $<$<STREQUAL:${EX_LANG},cpp>:-static-libstdc++>)
    endif ()
    _gmp_example_test(${name} ${EX_LANG} "${EX_EXPECTED}" COMMAND "$<TARGET_FILE:${name}>")
endfunction()

# One example in every language: gmp_add_example_all(<name> SCHEMA <yaml> MAIN <stem> EXPECTED <file> [FILES ...])
# adds <name> (main.cpp), <name>_c, <name>_js and <name>_py; FILES are extra modules for JS / Python (codecs).
function(gmp_add_example_all name)
    cmake_parse_arguments(A "" "SCHEMA;MAIN;EXPECTED" "JS_FILES;PY_FILES" ${ARGN})
    set(schema ${CMAKE_CURRENT_SOURCE_DIR}/${A_SCHEMA})
    gmp_add_example(${name} SCHEMAS ${schema} SOURCES ${A_MAIN}.cpp EXPECTED ${A_EXPECTED})
    gmp_add_example(${name}_c LANG c SCHEMAS ${schema} SOURCES ${A_MAIN}.c EXPECTED ${A_EXPECTED})
    gmp_add_example(${name}_js LANG js SCHEMAS ${schema} SOURCES ${A_MAIN}.js FILES ${A_JS_FILES} EXPECTED ${A_EXPECTED})
    gmp_add_example(${name}_py LANG python SCHEMAS ${schema} SOURCES ${A_MAIN}.py FILES ${A_PY_FILES} EXPECTED ${A_EXPECTED})
endfunction()

add_subdirectory(binary_telemetry)
add_subdirectory(nmea_text)
add_subdirectory(bitpacked_can)
add_subdirectory(custom_codec)
add_subdirectory(sensirion_sht4x)
add_subdirectory(reserved_sync)
