cmake_minimum_required(VERSION 3.20)
project(MeshLLM VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(meshllm src/main.cpp)
target_compile_features(meshllm PRIVATE cxx_std_20)
target_link_libraries(meshllm PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(meshllm PRIVATE /W4 /permissive-)
else()
  target_compile_options(meshllm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(meshllm PRIVATE ws2_32)
endif()

install(TARGETS meshllm RUNTIME DESTINATION bin)
