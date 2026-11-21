set(SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../schemas)

gmp_generate(TARGET gmp_test_schemas
        SCHEMAS
        ${SCHEMA_DIR}/telemetry.yaml
        ${SCHEMA_DIR}/nmea.yaml
        ${SCHEMA_DIR}/hdlc.yaml
        ${SCHEMA_DIR}/can.yaml
        ${SCHEMA_DIR}/custom.yaml
        ${SCHEMA_DIR}/fieldcrc.yaml
        ${SCHEMA_DIR}/enums.yaml
        ${SCHEMA_DIR}/computed.yaml
        ${SCHEMA_DIR}/stuffed.yaml
        ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/sensirion_sht4x/sht4x.yaml
        ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/sensirion_sht4x/sht4x_enum.yaml)
# custom codecs referenced by hdlc.yaml
target_include_directories(gmp_test_schemas INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

gmp_add_test(gmp_roundtrip_tests
        test_telemetry.cpp
        test_nmea.cpp
        test_hdlc_can.cpp
        test_custom.cpp
        test_fieldcrc.cpp
        test_enums.cpp
        test_computed.cpp
        test_sht4x.cpp
        test_stuffed.cpp
        test_print.cpp
        test_stream_callbacks.cpp
        test_stream_poll.cpp)
target_link_libraries(gmp_roundtrip_tests PRIVATE gmp_test_schemas)

# Embedded profile: generated code + runtime must build without exceptions / RTTI and never allocate.
add_executable(gmp_embedded_profile embedded_profile.cpp)
target_link_libraries(gmp_embedded_profile PRIVATE gmp_test_schemas)
target_compile_options(gmp_embedded_profile PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-exceptions -fno-rtti -Wall -Wextra -Werror>)
target_link_options(gmp_embedded_profile PRIVATE ${GMP_STATIC_LINK})
add_test(NAME gmp_embedded_profile COMMAND gmp_embedded_profile)

# Reference data for the Schema Studio cross-check (tools/schema-studio/tests): frames and gmp::print output.
add_executable(gmp_studio_crossref ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/schema-studio/tests/crossref.cpp)
target_link_libraries(gmp_studio_crossref PRIVATE gmp_test_schemas)
target_link_options(gmp_studio_crossref PRIVATE ${GMP_STATIC_LINK})

# Schema Studio tests (tools/schema-studio/tests): run in headless Edge / Chrome / Chromium.
# Reported as skipped (exit code 77) when no browser is found; tests needing js-yaml (cdnjs) skip when offline.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND AND TARGET gmpc)
    add_test(NAME gmp_studio_tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/schema-studio/tests/run_tests.py
            --gmpc $<TARGET_FILE:gmpc> --crossref $<TARGET_FILE:gmp_studio_crossref>)
    set_tests_properties(gmp_studio_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
else ()
    message(STATUS "GMP: Python 3 not found, Schema Studio tests are not registered")
endif ()
