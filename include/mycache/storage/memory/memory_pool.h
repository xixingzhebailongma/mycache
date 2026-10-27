#pragma once

#include <cstddef>
#include <cstdint>

namespace mycache {

// 池化分配层：拦截 <= 4096 字节的小块，按 size class 复用，省掉
// malloc/free/malloc_usable_size 的簿记。返回指针与 malloc 同为 16 字节对齐。
//
// 关键不变量：一块内存必须由「同一线程分配、同一线程释放」（thread_local 池）。
// 该不变量靠对象所有权设计保证（对象在归属线程创建/销毁，共享对象永不 free），
// 无运行时兜底：magic 只区分池块与真实 malloc 块，跨线程 free 不被识别，
// 会静默污染对方 freelist 且本线程丢块（不 crash，故更隐蔽）。

// 池上限：> 4096 一律直连（页边界，见 docs/memory-pool.md）。
inline constexpr uint32_t kMaxPoolSize = 4096;

// 池是否被 MYCACHE_DISABLE_POOL 关闭（A/B 用，同一二进制运行时切换）。
bool PoolDisabled();

// 分配一个池块（size: 1..kMaxPoolSize）。成功返回 16 对齐的用户指针，
// 并把实际采用的 class 写入 *out_class。失败返回 nullptr。
void* PoolAlloc(size_t size, uint32_t* out_class);

// 释放一个由 PoolAlloc 返回的池块。
void PoolFree(void* ptr);

// ptr 是否为池块（magic + class 区间校验）。是则写 *out_class 并返回 true。
bool PoolOwns(const void* ptr, uint32_t* out_class);

// 显式归还当前线程所有空闲池块给 OS。不影响 used_memory（空闲块已下账）。
// 必须在业务线程退出前、该线程不再有任何分配后调用（放在线程函数末尾）。
void PoolFlushAll();

}  // namespace mycache
