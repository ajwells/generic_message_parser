gmp_add_example(example_binary_telemetry SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/telemetry.yaml SOURCES main.cpp)
gmp_add_example(example_binary_telemetry_c LANG c SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/telemetry.yaml SOURCES main.c)
