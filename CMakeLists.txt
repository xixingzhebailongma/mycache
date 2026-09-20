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
option(MYCACHE_ENABLE_TSAN "Enable ThreadSanitizer (data-race detection)" OFF)
option(MYCACHE_USE_JEMALLOC "Link jemalloc for lower fragmentation + thread-cache" ON)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

# ---- 核心库 ----
file(GLOB_RECURSE MYCACHE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM MYCACHE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(mycache_core STATIC ${MYCACHE_SOURCES})
target_include_directories(mycache_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mycache_core PUBLIC Threads::Threads)

# ---- io_uring（可选）：探测 liburing，未安装则回退 posix ----
find_library(URING_LIBRARY uring)
if(URING_LIBRARY)
  target_compile_definitions(mycache_core PUBLIC MYCACHE_HAVE_IO_URING)
  target_link_libraries(mycache_core PUBLIC ${URING_LIBRARY})
  message(STATUS "liburing found — io_uring engine enabled")
else()
  message(STATUS "liburing not found — io_uring engine disabled (posix fallback)")
endif()

# ---- jemalloc（可选）：替换 glibc malloc，降低碎片率 + 每线程 tcache。
# 传输到所有链接 mycache_core 的可执行文件；zmalloc 的 malloc/free/malloc_usable_size
# 符号会被 jemalloc 透明拦截。TSan 构建不链接 jemalloc（TSan 需拦截分配器）。----
if(MYCACHE_USE_JEMALLOC AND NOT MYCACHE_ENABLE_TSAN AND NOT MYCACHE_ENABLE_SANITIZERS)
  # 同时匹配 dev 包（libjemalloc.so）与仅运行时包（libjemalloc.so.2）。find_library
  # 匹配不了带版本号的 .so.2 且默认不搜 /usr/lib/<multiarch>，故用 find_file + HINTS。
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-multiarch
    OUTPUT_VARIABLE _jemalloc_arch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(_jemalloc_hints)
  if(_jemalloc_arch)
    list(APPEND _jemalloc_hints /usr/lib/${_jemalloc_arch} /lib/${_jemalloc_arch})
  endif()
  find_file(JEMALLOC_LIBRARY NAMES libjemalloc.so libjemalloc.so.2 HINTS ${_jemalloc_hints})
  if(JEMALLOC_LIBRARY)
    target_link_libraries(mycache_core PUBLIC ${JEMALLOC_LIBRARY})
    message(STATUS "jemalloc found — linked (lower fragmentation, per-thread cache)")
  else()
    message(STATUS "jemalloc not found — falling back to glibc malloc")
  endif()
endif()

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
    tests/unit/info_test.cpp
    tests/unit/multi_key_test.cpp
    tests/unit/week6_test.cpp)
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
  # IOEngine write+fsync 压测（posix vs io_uring）。
  add_executable(mycache_io_bench benchmark/io_bench.cpp)
  target_link_libraries(mycache_io_bench PRIVATE mycache_core)
endif()
