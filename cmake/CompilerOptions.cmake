# 在 Linux 上启用 GNU 扩展（eventfd、accept4、localtime_r 等）。
add_compile_definitions(_GNU_SOURCE)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

if(MYCACHE_ENABLE_SANITIZERS)
  include(cmake/Sanitizer.cmake)
endif()

# ThreadSanitizer：数据竞争检测（Week 7 稳定性验收的「无数据竞争」）。
if(MYCACHE_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
