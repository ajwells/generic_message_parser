# Generated Python: every test schema is generated with gmpc --lang python and cross-checked against the generated C++
# (test_generated.py against the reference results of js_crossref, the same as the JavaScript cross-check). The
# Python user extensions of hdlc.yaml / custom.yaml (test_codecs.py) are imported from this directory. test_api.py
# covers the Python API with api.yaml (what the cross-check does not reach).
set(SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../schemas)
set(EXAMPLES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../examples)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)

gmp_generate(TARGET gmp_test_schemas_py LANG python OUT_DIR ${GEN_DIR}
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
        ${EXAMPLES_DIR}/reserved_sync/motor_link.yaml
        ${CMAKE_CURRENT_SOURCE_DIR}/api.yaml)
add_dependencies(gmp_js_crossref gmp_test_schemas_py)

if (GMP_PYTHON)
    add_test(NAME gmp_py_generated_tests
            COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_generated.py $<TARGET_FILE:gmp_js_crossref> ${GEN_DIR} --pure)
    add_test(NAME gmp_py_api_tests COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_api.py ${GEN_DIR} --pure)
    set_tests_properties(gmp_py_generated_tests gmp_py_api_tests PROPERTIES ENVIRONMENT "GMSG_PATH=${PROJECT_SOURCE_DIR}/runtime-py")
    if (GMP_PY_NATIVE_DIR)  # with the accelerator (tests/runtime_py/CMakeLists.txt)
        add_test(NAME gmp_py_generated_tests_native
                COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_generated.py $<TARGET_FILE:gmp_js_crossref> ${GEN_DIR} --native)
        add_test(NAME gmp_py_api_tests_native COMMAND ${GMP_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/test_api.py ${GEN_DIR} --native)
        set_tests_properties(gmp_py_generated_tests_native gmp_py_api_tests_native PROPERTIES
                ENVIRONMENT "GMSG_PATH=${GMP_PY_NATIVE_DIR}")
    endif ()
endif ()
