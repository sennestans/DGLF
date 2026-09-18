cmake_minimum_required(VERSION 3.28)
project(MeshLLM VERSION 0.2.0 LANGUAGES C CXX)

include(ExternalProject)
find_package(Threads REQUIRED)

if(APPLE)
  set(MESHLLM_BUNDLE_LLAMA_CPP_DEFAULT ON)
else()
  set(MESHLLM_BUNDLE_LLAMA_CPP_DEFAULT OFF)
endif()
option(MESHLLM_BUNDLE_LLAMA_CPP "Download and build a pinned llama.cpp"
  ${MESHLLM_BUNDLE_LLAMA_CPP_DEFAULT})

if(MESHLLM_BUNDLE_LLAMA_CPP)
  # Build llama.cpp as its own CMake project: current llama.cpp tooling assumes
  # it is the top-level source tree. Pinning keeps the CLI contract reproducible.
  set(MESHLLM_LLAMA_PREFIX "${CMAKE_BINARY_DIR}/_deps/llama_cpp")
  set(MESHLLM_LLAMA_CLI "${MESHLLM_LLAMA_PREFIX}/src/llama_cpp_external-build/bin/llama-cli")
  ExternalProject_Add(llama_cpp_external
    PREFIX "${MESHLLM_LLAMA_PREFIX}"
    URL https://github.com/ggml-org/llama.cpp/archive/refs/tags/v0.4.1.tar.gz
    URL_HASH SHA256=ef3d5b1907a391500ae11b5e61a8e2022e0deaac9790899cad9c4e02f03bfb9a
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DBUILD_SHARED_LIBS=OFF
      -DLLAMA_BUILD_TESTS=OFF
      -DLLAMA_BUILD_EXAMPLES=OFF
      -DLLAMA_BUILD_SERVER=ON
      -DLLAMA_BUILD_APP=OFF
      -DLLAMA_BUILD_UI=OFF
      -DLLAMA_USE_PREBUILT_UI=OFF
      -DLLAMA_TOOLS_INSTALL=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target llama-cli --parallel 4
    BUILD_BYPRODUCTS "${MESHLLM_LLAMA_CLI}"
    INSTALL_COMMAND ""
  )
endif()

add_executable(meshllm src/main.cpp)
target_compile_features(meshllm PRIVATE cxx_std_20)
target_link_libraries(meshllm PRIVATE Threads::Threads)

if(MESHLLM_BUNDLE_LLAMA_CPP)
  add_dependencies(meshllm llama_cpp_external)
  target_compile_definitions(meshllm PRIVATE
    MESHLLM_BUNDLED_LLAMA_CLI="${MESHLLM_LLAMA_CLI}")
endif()

if(MSVC)
  target_compile_options(meshllm PRIVATE /W4 /permissive-)
else()
  target_compile_options(meshllm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(meshllm PRIVATE ws2_32)
endif()

install(TARGETS meshllm RUNTIME DESTINATION bin)

if(MESHLLM_BUNDLE_LLAMA_CPP)
  install(PROGRAMS "${MESHLLM_LLAMA_CLI}" DESTINATION bin)
endif()
