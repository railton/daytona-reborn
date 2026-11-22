cmake_minimum_required(VERSION 3.16)
project(race_start_failure LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_race_start_failure test.cpp)
target_include_directories(test_race_start_failure PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools")
add_test(NAME test_race_start_failure COMMAND test_race_start_failure)
