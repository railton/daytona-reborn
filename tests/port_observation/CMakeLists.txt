cmake_minimum_required(VERSION 3.16)
project(port_observation_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(port_observation_test test.cpp)
target_compile_features(port_observation_test PRIVATE cxx_std_17)
target_include_directories(
    port_observation_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
)

enable_testing()
add_test(NAME port_observation_test COMMAND port_observation_test)

add_executable(port_board_test board.cpp ../../src/runtime/m2_tgp_board.cpp ../../src/runtime/tgp.cpp)
target_compile_features(port_board_test PRIVATE cxx_std_20)
target_include_directories(port_board_test PRIVATE ../../src)
add_test(NAME port_board_test COMMAND port_board_test)
