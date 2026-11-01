cmake_minimum_required(VERSION 3.16)
project(Workbench VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Flags string recorded in the benchmark report.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
set(WB_FLAGS_STR "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}}")
string(STRIP "${WB_FLAGS_STR}" WB_FLAGS_STR)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- core: pure C++ (no HTTP, no JSON) --------------------------------------
file(GLOB WB_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
add_library(wb_core STATIC ${WB_CORE_SRC})
target_include_directories(wb_core PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(wb_core PUBLIC Threads::Threads)
target_compile_definitions(wb_core PRIVATE
  WB_BUILD_FLAGS="${WB_FLAGS_STR}" WB_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# ---- server: HTTP routes, JSON mapping, job manager ------------------------
file(GLOB WB_SERVER_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/server/*.cpp)
add_library(wb_server STATIC ${WB_SERVER_SRC})
target_include_directories(wb_server PUBLIC ${CMAKE_SOURCE_DIR}/src/server
                                     SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(wb_server PUBLIC wb_core)
if(WIN32)
  target_link_libraries(wb_server PUBLIC ws2_32 crypt32)
  target_compile_definitions(wb_server PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---- Launch: the one-click app ---------------------------------------------
add_executable(Launch src/app/main.cpp)
target_link_libraries(Launch PRIVATE wb_server)
if(WIN32)
  target_link_libraries(Launch PRIVATE shell32)
  if(MINGW)
    target_link_options(Launch PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
  endif()
endif()
# Copy the frontend next to the binary so a build-tree run works too.
# A separate always-run target: a POST_BUILD step only fires when Launch
# relinks, which left a stale web/ after frontend-only edits.
add_custom_target(copy_web ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/src/frontend $<TARGET_FILE_DIR:Launch>/web
  COMMENT "Copying frontend to web/")
add_dependencies(copy_web Launch)

# ---- tools: C++ CLIs that regenerate results/*.csv and *.svg ---------------
file(GLOB WB_TOOL_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/*.cpp)
foreach(src ${WB_TOOL_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tools)
  target_link_libraries(${name} PRIVATE wb_core)
endforeach()

# ---- tests ------------------------------------------------------------------
enable_testing()
file(GLOB WB_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(wb_tests ${WB_TEST_SRC})
target_include_directories(wb_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(wb_tests PRIVATE wb_server)
target_compile_definitions(wb_tests PRIVATE WB_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
add_test(NAME wb_tests COMMAND wb_tests)
