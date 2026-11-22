cmake_minimum_required(VERSION 3.10)
project(write_provenance LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(write_provenance_test test.cpp)
target_include_directories(write_provenance_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME write_provenance COMMAND write_provenance_test)
