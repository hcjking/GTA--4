# [web port] Builds ReXGlue's vendored third-party libraries for WebAssembly
# (Emscripten, memory64 + pthreads) by reusing rexglue's own thirdparty list.
cmake_minimum_required(VERSION 3.25)
project(gta4-web-thirdparty LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

set(REXGLUE_USE_VULKAN OFF)
set(REXGLUE_USE_D3D12 OFF)
set(REXGLUE_ENABLE_TRACY OFF)
set(REXGLUE_BUILD_TESTS OFF)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../glue/rexglue-sdk-main")
add_subdirectory("${REXSDK_DIR}/thirdparty" tp)
