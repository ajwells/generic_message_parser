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
        src/backend/c/emit_text_message.cpp
        src/backend/js/js_backend.cpp
        src/backend/js/js_common.cpp
        src/backend/js/emit_binary_message.cpp
        src/backend/js/emit_text_message.cpp
        src/backend/js/emit_protocol.cpp
        src/backend/py/py_backend.cpp
        src/backend/py/py_common.cpp
        src/backend/py/emit_binary_message.cpp
        src/backend/py/emit_text_message.cpp
        src/backend/py/emit_protocol.cpp)
target_include_directories(gmpc_lib PUBLIC src)
target_link_libraries(gmpc_lib PUBLIC yaml-cpp::yaml-cpp)
target_compile_options(gmpc_lib PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

add_executable(gmpc src/main.cpp)
target_link_libraries(gmpc PRIVATE gmpc_lib)
if (MINGW)
    target_link_options(gmpc PRIVATE -static-libgcc -static-libstdc++)
endif ()

# gmpc as a WebAssembly module for Schema Studio (tools/schema-studio/gmpc/gmpc.js): built with Emscripten
# (emcmake), normally through tools/schema-studio/gmpc/build.py, which also records the source stamp.
if (EMSCRIPTEN)
    set(GMPC_SOURCE_STAMP "unknown" CACHE STRING "Hash of the generator sources the WebAssembly module is built from")
    add_executable(gmpc_wasm src/wasm_main.cpp)
    target_link_libraries(gmpc_wasm PRIVATE gmpc_lib)
    target_compile_definitions(gmpc_wasm PRIVATE GMPC_SOURCE_STAMP="${GMPC_SOURCE_STAMP}")
    target_link_options(gmpc_wasm PRIVATE
            -sSINGLE_FILE=1 -sMODULARIZE=1 -sEXPORT_NAME=createGmpc -sENVIRONMENT=web,node
            -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS=_gmpc_generate,_gmpc_version
            -sEXPORTED_RUNTIME_METHODS=ccall -fexceptions)
    set_target_properties(gmpc_wasm PROPERTIES SUFFIX ".js")
endif ()
