set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG yaml-cpp-0.9.0
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(yaml-cpp)

# Library with everything except main() so tests can drive the generator in-process.
add_library(gmpc_lib STATIC
        src/schema/expr.cpp
        src/schema/loader.cpp
        src/schema/validator.cpp
        src/ir/ir.cpp
        src/ir/lowering.cpp
        src/backend/registry.cpp
        src/backend/cpp/cpp_backend.cpp
        src/backend/cpp/cpp_common.cpp
        src/backend/cpp/emit_binary_message.cpp
        src/backend/cpp/emit_text_message.cpp
        src/backend/cpp/emit_protocol.cpp
        src/backend/c/c_backend.cpp
        src/backend/c/c_common.cpp
        src/backend/c/emit_binary_message.cpp
        src/backend/c/emit_protocol.cpp
        src/backend/c/emit_text_message.cpp)
target_include_directories(gmpc_lib PUBLIC src)
target_link_libraries(gmpc_lib PUBLIC yaml-cpp::yaml-cpp)
target_compile_options(gmpc_lib PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_executable(gmpc src/main.cpp)
target_link_libraries(gmpc PRIVATE gmpc_lib)
if (MINGW)
    target_link_options(gmpc PRIVATE -static-libgcc -static-libstdc++)
endif ()
