cmake_minimum_required(VERSION 3.16)
project(checked_trace_file_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(checked_trace_file_test test.cpp)
target_include_directories(checked_trace_file_test PRIVATE "${REPO_ROOT}/src")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
  target_link_libraries(checked_trace_file_test PRIVATE stdc++fs)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME checked_trace_file COMMAND checked_trace_file_test)

# Argument vector, not a shell. Python owns the temp dir so an abort in the
# child still deletes only that directory. Nonzero child status is success.
set(CHECKED_TRACE_FILE_DTOR_PY [=[
import os, shutil, subprocess, sys, tempfile
exe = sys.argv[1]
d = tempfile.mkdtemp(prefix="checked_trace_file_dtor_")
code = 1
try:
    trace = os.path.join(d, "trace.bin")
    result = subprocess.run([exe, "--destructor-close-failure", trace], capture_output=True, timeout=30)
    code = 0 if result.returncode != 0 and b"rt::CheckedTraceFile: close failed" in result.stderr else 1
finally:
    shutil.rmtree(d, ignore_errors=True)
raise SystemExit(code)
]=])

add_test(
  NAME checked_trace_file_destructor_close_failure
  COMMAND Python3::Interpreter -c "${CHECKED_TRACE_FILE_DTOR_PY}"
          $<TARGET_FILE:checked_trace_file_test>)
