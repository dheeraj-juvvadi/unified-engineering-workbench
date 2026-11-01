# Cross-compile Launch.exe for Windows x64 from Linux/macOS with MinGW-w64
# (posix thread model, so std::thread works).
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT WB_MINGW_PREFIX)
  set(WB_MINGW_PREFIX x86_64-w64-mingw32)
endif()
find_program(_wb_cxx NAMES ${WB_MINGW_PREFIX}-g++-posix ${WB_MINGW_PREFIX}-g++ REQUIRED)
set(CMAKE_CXX_COMPILER ${_wb_cxx})
set(CMAKE_RC_COMPILER ${WB_MINGW_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Fully static so Launch.exe needs no MinGW DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
