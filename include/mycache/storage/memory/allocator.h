#pragma once

#include <cstddef>

namespace mycache {

// 带内存统计的底层分配器（模仿 Redis 的 zmalloc）。所有引擎内部的持久化内存
// （sds、dict 表/条目、对象）都应经过这里分配，以便 used_memory() 统计堆占用。
//

void *zmalloc(size_t size);
void *zcalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);

// 当前已分配字节数（原子读取，跨线程安全）。
size_t used_memory();

} // namespace mycache
