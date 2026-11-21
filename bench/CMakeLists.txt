gmp_generate(TARGET gmp_bench_schemas SCHEMAS
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/schemas/telemetry.yaml
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/schemas/nmea.yaml)
add_executable(gmp_bench bench.cpp)
target_link_libraries(gmp_bench PRIVATE gmp_bench_schemas)
if (MINGW)
    target_link_options(gmp_bench PRIVATE -static-libgcc -static-libstdc++)
endif ()
