cmake_minimum_required(VERSION 3.25)

project(svr07 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(SVR07_SOURCES
  src/main.cpp
  src/svr07_hooks.cpp
  src/svr07_portraits.cpp
  src/svr07_roster.cpp
  src/svr07_arenas.cpp
)

if(WIN32)
  add_executable(svr07 WIN32 ${SVR07_SOURCES})
else()
  add_executable(svr07 ${SVR07_SOURCES})
endif()

rexglue_setup_target(svr07 GPU_PLUGINS xenos)
# Command-line build of the portrait-merged menu pacs (same code as the game).
add_executable(svr07_portraits src/svr07_portraits.cpp src/svr07_portraits_tool.cpp)
target_link_libraries(svr07_portraits PRIVATE rex::runtime)
