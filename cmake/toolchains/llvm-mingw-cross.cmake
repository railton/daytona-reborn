# Cross-compiling for Windows (x86_64) from Linux with llvm-mingw
# (docker/windows-cross). On Windows itself llvm-mingw's clang already
# targets Windows and needs no toolchain file (scripts/windows-build.ps1).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${triple}-clang)
set(CMAKE_CXX_COMPILER ${triple}-clang++)
set(CMAKE_RC_COMPILER ${triple}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
