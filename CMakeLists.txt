cmake_minimum_required(VERSION 3.17)
project(cpp-pi CXX)

set(CMAKE_CXX_STANDARD 20)   # folly 需要 C++20; trans-dsl-2 在 C++20 下验证通过
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
# vendored 三方库 (third_party/): folly(JSON) + fmt + glog
# ---------------------------------------------------------------------------
set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fmt)

set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_UNWIND OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glog)

# folly 最小子集: json/dynamic 及其闭包 —— 缺符号时按链接错误补源文件
add_library(folly_json STATIC
   third_party/folly/folly/json/json.cpp
   third_party/folly/folly/json/dynamic.cpp
   third_party/folly/folly/json/DynamicParser.cpp
   third_party/folly/folly/Conv.cpp
   third_party/folly/folly/Unicode.cpp
   third_party/folly/folly/ScopeGuard.cpp
   third_party/folly/folly/lang/SafeAssert.cpp
   third_party/folly/folly/lang/ToAscii.cpp
   third_party/folly/folly/json/json_pointer.cpp
   third_party/folly/folly/container/detail/F14Table.cpp
   third_party/folly/folly/hash/SpookyHashV2.cpp)
target_include_directories(folly_json PUBLIC
   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/folly"
   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/folly-config"
   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost-headers"
   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fast_float/include")
target_link_libraries(folly_json PUBLIC fmt::fmt glog::glog)

add_subdirectory(third_party/quill)

# ---------------------------------------------------------------------------
# cpp-pi
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CPP_PI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_executable(cpp-pi ${CPP_PI_SOURCES})
target_include_directories(cpp-pi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cpp-pi PRIVATE trans-dsl folly_json quill)
