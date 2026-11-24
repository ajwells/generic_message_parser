# Generated JavaScript: every test schema is generated with gmpc --lang js and cross-checked against the generated
# C++ under Node.js (test_generated.js against the reference results of js_crossref). The JS user extensions of
# hdlc.yaml / custom.yaml (test_codecs.js) are copied next to the generated modules.
set(SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../schemas)
set(EXAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../examples)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)

gmp_generate(TARGET gmp_test_schemas_js LANG js OUT_DIR ${GEN_DIR}
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
add_custom_command(
        OUTPUT ${GEN_DIR}/test_codecs.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/test_codecs.js ${GEN_DIR}/test_codecs.js
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_codecs.js
        VERBATIM)
add_custom_target(gmp_test_codecs_js ALL DEPENDS ${GEN_DIR}/test_codecs.js)

add_executable(gmp_js_crossref js_crossref.cpp)
target_link_libraries(gmp_js_crossref PRIVATE gmp_test_schemas)
target_link_options(gmp_js_crossref PRIVATE ${GMP_STATIC_LINK})
add_dependencies(gmp_js_crossref gmp_test_schemas_js gmp_test_codecs_js)

if (GMP_NODE)
    add_test(NAME gmp_js_generated_tests
            COMMAND ${GMP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_generated.js $<TARGET_FILE:gmp_js_crossref> ${GEN_DIR})
    add_test(NAME gmp_js_endpoint_tests
            COMMAND ${GMP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_endpoint.js ${GEN_DIR})
endif ()
