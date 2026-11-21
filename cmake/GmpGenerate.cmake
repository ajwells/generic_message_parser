# gmp_generate(TARGET <name>
#              SCHEMAS <file.yaml>...
#              [OUT_DIR <dir>]          # default: ${CMAKE_CURRENT_BINARY_DIR}/gmp_generated/<name>
#              [LANG <backend>]         # default: cpp
#              [GENERATOR <exe>])       # default: the in-tree gmpc target
#
# Creates an INTERFACE library <name> exposing the generated headers (<schema-stem>.gmp.hpp) and
# linking gmp::runtime. Code is regenerated whenever a schema or the generator changes.

function(gmp_generate)
    cmake_parse_arguments(GMP "" "TARGET;OUT_DIR;LANG;GENERATOR" "SCHEMAS" ${ARGN})
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

    set(outputs)
    foreach (schema IN LISTS GMP_SCHEMAS)
        get_filename_component(abs "${schema}" ABSOLUTE)
        get_filename_component(stem "${schema}" NAME_WE)
        set(out "${GMP_OUT_DIR}/${stem}.gmp.hpp")
        add_custom_command(
                OUTPUT "${out}"
                COMMAND ${GMP_GENERATOR} --lang ${GMP_LANG} --out "${GMP_OUT_DIR}" "${abs}"
                DEPENDS "${abs}" ${GMP_GENERATOR_DEP}
                COMMENT "gmpc: generating ${stem}.gmp.hpp"
                VERBATIM)
        list(APPEND outputs "${out}")
    endforeach ()

    add_custom_target(${GMP_TARGET}_codegen DEPENDS ${outputs})
    add_library(${GMP_TARGET} INTERFACE)
    add_dependencies(${GMP_TARGET} ${GMP_TARGET}_codegen)
    target_include_directories(${GMP_TARGET} INTERFACE "${GMP_OUT_DIR}")
    target_link_libraries(${GMP_TARGET} INTERFACE gmp::runtime)
    set_property(TARGET ${GMP_TARGET} PROPERTY GMP_GENERATED_HEADERS "${outputs}")
endfunction()
