cmake_minimum_required(VERSION 3.16)
project(probe_evidence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(probe_evidence_test test.cpp)
target_include_directories(probe_evidence_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools")
add_test(NAME probe_evidence_test COMMAND probe_evidence_test)
