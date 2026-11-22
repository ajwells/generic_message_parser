gmp_add_example(example_custom_codec SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/meter.yaml SOURCES main.cpp)
gmp_add_example(example_custom_codec_c LANG c SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/meter.yaml SOURCES main.c)
