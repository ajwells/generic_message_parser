# Generated C code: every schema the C backend supports is generated and compiled as strict C11 (warnings are
# errors), then cross-checked against the generated C++ (test_c_generated.cpp). The same generated files are also
# compiled as C99 (header-only runtime: compile check) and as C99 against the library build of the runtime
# (gmp::runtime_c_lib), whose cross-check runs as well.
set(SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../schemas)
set(EXAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../examples)

gmp_generate(TARGET gmp_test_schemas_c LANG c
        SCHEMAS
        ${SCHEMA_DIR}/telemetry.yaml
        ${SCHEMA_DIR}/nmea.yaml
        ${SCHEMA_DIR}/hdlc.yaml
        ${SCHEMA_DIR}/can.yaml
        ${SCHEMA_DIR}/custom.yaml
        ${SCHEMA_DIR}/fieldcrc.yaml
        ${SCHEMA_DIR}/enums.yaml
        ${SCHEMA_DIR}/limits.yaml
        ${SCHEMA_DIR}/computed.yaml
        ${SCHEMA_DIR}/stuffed.yaml
        ${SCHEMA_DIR}/framing.yaml
        ${EXAMPLES_DIR}/sensirion_sht4x/sht4x.yaml
        ${EXAMPLES_DIR}/sensirion_sht4x/sht4x_enum.yaml
        ${EXAMPLES_DIR}/reserved_sync/motor_link.yaml)
set_target_properties(gmp_test_schemas_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(gmp_test_schemas_c PRIVATE ${GMP_C_WARNINGS})
# C user extensions referenced by the c: blocks of hdlc.yaml and custom.yaml
target_include_directories(gmp_test_schemas_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

gmp_add_test(gmp_c_generated_tests test_c_generated.cpp test_c_protocols.cpp test_c_text.cpp)
target_link_libraries(gmp_c_generated_tests PRIVATE gmp_test_schemas_c gmp_test_schemas)
# test_c_protocols.cpp instantiates every check for ~25 protocols: more sections than a default COFF object holds
if (MINGW)
    set_source_files_properties(test_c_protocols.cpp PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif ()

get_target_property(c_sources gmp_test_schemas_c GMP_GENERATED_SOURCES)
get_target_property(c_include gmp_test_schemas_c INCLUDE_DIRECTORIES)
foreach (variant IN ITEMS c99 c99_lib)
    set(lib gmp_test_schemas_${variant})
    add_library(${lib} STATIC ${c_sources})
    add_dependencies(${lib} gmp_test_schemas_c_codegen)
    set_target_properties(${lib} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${lib} PRIVATE ${GMP_C_WARNINGS})
    target_include_directories(${lib} PUBLIC ${c_include})
endforeach ()
target_link_libraries(gmp_test_schemas_c99 PUBLIC gmp::runtime_c)
target_link_libraries(gmp_test_schemas_c99_lib PUBLIC gmp::runtime_c_lib)

gmp_add_test(gmp_c_generated_tests_lib test_c_generated.cpp test_c_protocols.cpp test_c_text.cpp
        TEST_SUFFIX " [c99, runtime_c_lib]")
target_link_libraries(gmp_c_generated_tests_lib PRIVATE gmp_test_schemas_c99_lib gmp_test_schemas)
