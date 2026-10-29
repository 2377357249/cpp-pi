cmake_minimum_required(VERSION 3.17)
project(cpp-pi CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# trans-dsl-2: 默认取兄弟目录 ../trans-dsl-2, 可用 -DTDSL_ROOT=<path> 覆盖
# ---------------------------------------------------------------------------
get_filename_component(TDSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../trans-dsl-2" ABSOLUTE)
if(NOT EXISTS "${TDSL_ROOT}/include/trans-dsl/trans-dsl.h")
   message(FATAL_ERROR
      "trans-dsl-2 not found at ${TDSL_ROOT}\n"
      "clone it next to cpp-pi, or pass -DTDSL_ROOT=<path-to-trans-dsl-2>")
endif()

file(GLOB_RECURSE TDSL_SOURCES CONFIGURE_DEPENDS "${TDSL_ROOT}/src/*.cpp")
add_library(trans-dsl STATIC ${TDSL_SOURCES})
target_include_directories(trans-dsl PUBLIC
   "${TDSL_ROOT}/include"
   "${TDSL_ROOT}/third-party")

# ---------------------------------------------------------------------------
# cpp-pi
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CPP_PI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_executable(cpp-pi ${CPP_PI_SOURCES})
target_include_directories(cpp-pi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cpp-pi PRIVATE trans-dsl)
