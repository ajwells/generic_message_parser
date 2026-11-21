FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

if (MINGW)
    # Keep test binaries self-contained (no dependency on MinGW DLLs being on PATH).
    set(GMP_STATIC_LINK -static-libgcc -static-libstdc++)
endif ()

function(gmp_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain gmp::runtime)
    target_compile_options(${name} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>)
    target_link_options(${name} PRIVATE ${GMP_STATIC_LINK})
    catch_discover_tests(${name})
endfunction()

add_subdirectory(runtime)
if (GMP_BUILD_GENERATOR)
    add_subdirectory(generator)
    add_subdirectory(roundtrip)
endif ()
