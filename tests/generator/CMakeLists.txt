add_executable(gmp_generator_tests test_diagnostics.cpp test_golden.cpp test_expr.cpp)
target_link_libraries(gmp_generator_tests PRIVATE gmpc_lib Catch2::Catch2WithMain)
target_link_options(gmp_generator_tests PRIVATE ${GMP_STATIC_LINK})
target_compile_definitions(gmp_generator_tests PRIVATE
        GMP_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../schemas"
        GMP_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
catch_discover_tests(gmp_generator_tests)
