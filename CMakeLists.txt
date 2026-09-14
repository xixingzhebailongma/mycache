cmake_minimum_required(VERSION 3.16)
project(mycache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 导出 compile_commands.json，供 clangd 等语言服务器获取每个文件的 -I/-std 编译参数。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MYCACHE_BUILD_TESTS "Build unit tests" ON)
option(MYCACHE_BUILD_BENCH "Build benchmark client" ON)
option(MYCACHE_ENABLE_SANITIZERS "Enable ASan/UBSan" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

# ---- 核心库 ----
file(GLOB_RECURSE MYCACHE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM MYCACHE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(mycache_core STATIC ${MYCACHE_SOURCES})
target_include_directories(mycache_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mycache_core PUBLIC Threads::Threads)

# ---- 服务端可执行文件 ----
add_executable(mycache-server src/main.cpp)
target_link_libraries(mycache-server PRIVATE mycache_core)

# ---- 单元测试 ----
if(MYCACHE_BUILD_TESTS)
  enable_testing()
  add_executable(mycache_tests
    tests/unit/resp_test.cpp
    tests/unit/dict_test.cpp
    tests/unit/sds_test.cpp
    tests/unit/object_test.cpp
    tests/unit/rehash_test.cpp
    tests/unit/hash_test.cpp
    tests/unit/ttl_test.cpp
    tests/unit/router_test.cpp
    tests/unit/aof_test.cpp
    tests/unit/lru_test.cpp
    tests/unit/eviction_test.cpp
    tests/unit/list_test.cpp
    tests/unit/info_test.cpp)
  target_link_libraries(mycache_tests PRIVATE mycache_core GTest::gtest GTest::gtest_main)
  add_test(NAME mycache_tests COMMAND mycache_tests)
endif()

# ---- 基准测试客户端 ----
if(MYCACHE_BUILD_BENCH)
  add_executable(mycache_bench benchmark/pipeline_test.cpp)
  # 百万级 key 内存引擎压测（绕过网络，直接驱动 Keyspace）。
  add_executable(mycache_stress benchmark/million_keys.cpp)
  target_link_libraries(mycache_stress PRIVATE mycache_core)
  # AOF 三种 fsync 策略压测（绕过网络，直接驱动 AofWriter）。
  add_executable(mycache_aof_bench benchmark/aof_bench.cpp)
  target_link_libraries(mycache_aof_bench PRIVATE mycache_core)
endif()
