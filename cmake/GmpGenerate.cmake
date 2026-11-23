# gmp_generate(TARGET <name>
#              SCHEMAS <file.yaml>...
#              [OUT_DIR <dir>]          # default: ${CMAKE_CURRENT_BINARY_DIR}/gmp_generated/<name>
#              [LANG <backend>]         # default: cpp
#              [C_RUNTIME header|library]  # LANG c: link gmp::runtime_c (header-only, default) or
#                                          # gmp::runtime_c_lib (compiled library, CRC tables)
#              [GENERATOR <exe>])       # default: the in-tree gmpc target
#
# Creates a library <name> exposing the generated code and linking the language's GMP runtime.
# Code is regenerated whenever a schema or the generator changes.
#
# Languages whose output is headers only (cpp: <schema-stem>.gmp.hpp) give an INTERFACE library;
# languages that also emit source files get a STATIC library built from them.
# LANG js writes <schema-stem>.gmp.js modules and copies the runtime (runtime-js/gmsg.js) next to them, so
# require('./gmsg.js') resolves; <name> is then a custom target (property GMP_GENERATED_DIR: the output directory).
# LANG python writes <schema-stem>_gmp.py modules that import the runtime package gmsg (runtime-py; put it on
# sys.path / PYTHONPATH or install it); <name> is a custom target with properties GMP_GENERATED_DIR and GMP_RUNTIME_DIR.

set(_GMP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Output layout and runtime target of each backend. Add a branch here when a backend is added to
# generator/src/backend/registry.cpp; the file names must match what the backend writes.
#   _GMP_HEADERS / _GMP_SOURCES: file name suffixes appended to the schema stem
#   _GMP_RUNTIME: runtime library target to link
function(_gmp_language_layout lang)
    if (lang STREQUAL "cpp")
        set(headers ".gmp.hpp")
        set(sources "")
        set(runtime gmp::runtime)
    elseif (lang STREQUAL "c")
        # C99 or later: <stem>.gmp.h + <stem>.gmp.c, using runtime-c (gmp/c/runtime.h)
        set(headers ".gmp.h")
        set(sources ".gmp.c")
        set(runtime gmp::runtime_c)
    elseif (lang STREQUAL "js")
        # JavaScript modules (UMD) using runtime-js/gmsg.js; nothing to compile
        set(headers "")
        set(sources ".gmp.js")
        set(runtime "")
    elseif (lang STREQUAL "python")
        # Python modules <stem>_gmp.py using the runtime package gmsg (runtime-py); nothing to compile
        set(headers "")
        set(sources "_gmp.py")
        set(runtime "")
    else ()
        message(FATAL_ERROR "gmp_generate: unsupported LANG '${lang}' (known: cpp, c, js, python)")
    endif ()
    set(_GMP_HEADERS "${headers}" PARENT_SCOPE)
    set(_GMP_SOURCES "${sources}" PARENT_SCOPE)
    set(_GMP_RUNTIME "${runtime}" PARENT_SCOPE)
endfunction()

function(gmp_generate)
    cmake_parse_arguments(GMP "" "TARGET;OUT_DIR;LANG;GENERATOR;C_RUNTIME" "SCHEMAS" ${ARGN})
    if (NOT GMP_TARGET)
        message(FATAL_ERROR "gmp_generate: TARGET is required")
    endif ()
    if (NOT GMP_SCHEMAS)
        message(FATAL_ERROR "gmp_generate: SCHEMAS is required")
    endif ()
    if (NOT GMP_OUT_DIR)
        set(GMP_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/gmp_generated/${GMP_TARGET}")
    endif ()
    if (NOT GMP_LANG)
        set(GMP_LANG cpp)
    endif ()
    if (NOT GMP_GENERATOR)
        if (NOT TARGET gmpc)
            message(FATAL_ERROR "gmp_generate: no gmpc target; pass GENERATOR <path>")
        endif ()
        set(GMP_GENERATOR $<TARGET_FILE:gmpc>)
        set(GMP_GENERATOR_DEP gmpc)
    endif ()
    _gmp_language_layout(${GMP_LANG})
    if (GMP_C_RUNTIME)
        if (NOT GMP_LANG STREQUAL "c")
            message(FATAL_ERROR "gmp_generate: C_RUNTIME is only for LANG c")
        elseif (GMP_C_RUNTIME STREQUAL "library")
            if (NOT TARGET gmp::runtime_c_lib)
                message(FATAL_ERROR "gmp_generate: C_RUNTIME library needs gmp::runtime_c_lib (enable the C language "
                        "before adding GMP)")
            endif ()
            set(_GMP_RUNTIME gmp::runtime_c_lib)
        elseif (NOT GMP_C_RUNTIME STREQUAL "header")
            message(FATAL_ERROR "gmp_generate: C_RUNTIME must be header or library, not '${GMP_C_RUNTIME}'")
        endif ()
    endif ()

    set(headers)
    set(sources)
    foreach (schema IN LISTS GMP_SCHEMAS)
        get_filename_component(abs "${schema}" ABSOLUTE)
        get_filename_component(stem "${schema}" NAME_WE)
        set(schema_outputs)
        foreach (suffix IN LISTS _GMP_HEADERS)
            list(APPEND schema_outputs "${GMP_OUT_DIR}/${stem}${suffix}")
            list(APPEND headers "${GMP_OUT_DIR}/${stem}${suffix}")
        endforeach ()
        foreach (suffix IN LISTS _GMP_SOURCES)
            list(APPEND schema_outputs "${GMP_OUT_DIR}/${stem}${suffix}")
            list(APPEND sources "${GMP_OUT_DIR}/${stem}${suffix}")
        endforeach ()
        add_custom_command(
                OUTPUT ${schema_outputs}
                COMMAND ${GMP_GENERATOR} --lang ${GMP_LANG} --out "${GMP_OUT_DIR}" "${abs}"
                DEPENDS "${abs}" ${GMP_GENERATOR_DEP}
                COMMENT "gmpc (${GMP_LANG}): generating code for ${stem}"
                VERBATIM)
    endforeach ()

    if (GMP_LANG STREQUAL "js")
        set(runtime_js "${_GMP_SOURCE_ROOT}/runtime-js/gmsg.js")
        add_custom_command(
                OUTPUT "${GMP_OUT_DIR}/gmsg.js"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_js}" "${GMP_OUT_DIR}/gmsg.js"
                DEPENDS "${runtime_js}"
                COMMENT "gmp: copying the JavaScript runtime"
                VERBATIM)
        add_custom_target(${GMP_TARGET} ALL DEPENDS ${sources} "${GMP_OUT_DIR}/gmsg.js")
        set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_SOURCES "${sources}")
        set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_DIR "${GMP_OUT_DIR}")
        return()
    endif ()
    if (GMP_LANG STREQUAL "python")
        add_custom_target(${GMP_TARGET} ALL DEPENDS ${sources})
        set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_SOURCES "${sources}")
        set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_DIR "${GMP_OUT_DIR}")
        set_property(TARGET ${GMP_TARGET} PROPERTY GMP_RUNTIME_DIR "${_GMP_SOURCE_ROOT}/runtime-py")
        return()
    endif ()

    add_custom_target(${GMP_TARGET}_codegen DEPENDS ${headers} ${sources})
    if (sources)
        add_library(${GMP_TARGET} STATIC ${sources})
        set(scope PUBLIC)
    else ()
        add_library(${GMP_TARGET} INTERFACE)
        set(scope INTERFACE)
    endif ()
    add_dependencies(${GMP_TARGET} ${GMP_TARGET}_codegen)
    target_include_directories(${GMP_TARGET} ${scope} "${GMP_OUT_DIR}")
    target_link_libraries(${GMP_TARGET} ${scope} ${_GMP_RUNTIME})
    set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_HEADERS "${headers}")
    set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_SOURCES "${sources}")
endfunction()
