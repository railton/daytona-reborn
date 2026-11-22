cmake_minimum_required(VERSION 3.20)
project(home_start_failure LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(extracted "${CMAKE_CURRENT_BINARY_DIR}/begin_solo.inc")
add_custom_command(OUTPUT "${extracted}"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_begin_solo.py" "${ROOT}/src/app/main.cpp" "${extracted}"
 DEPENDS extract_begin_solo.py "${ROOT}/src/app/main.cpp" VERBATIM)
add_executable(home_start_failure main.cpp "${extracted}")
target_include_directories(home_start_failure PRIVATE "${ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME home_start_failure COMMAND home_start_failure)
