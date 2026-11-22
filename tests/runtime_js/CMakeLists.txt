# JavaScript runtime tests (runtime-js/gmsg.js) under Node.js: unit checks plus a cross-check against the C++
# runtime (js_vectors prints the reference results). Node comes from PATH or an emsdk install; without it the test
# is not registered.

add_executable(gmp_js_vectors js_vectors.cpp)
target_link_libraries(gmp_js_vectors PRIVATE gmp::runtime gmp::runtime_c)
target_link_options(gmp_js_vectors PRIVATE ${GMP_STATIC_LINK})

if (GMP_NODE)
    add_test(NAME gmp_js_runtime_tests
            COMMAND ${GMP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime.js $<TARGET_FILE:gmp_js_vectors>)
else ()
    message(STATUS "GMP: Node.js not found, JavaScript runtime tests are disabled")
endif ()
