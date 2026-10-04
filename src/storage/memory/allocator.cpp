#include "mycache/storage/memory/allocator.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <malloc.h>

#include "mycache/storage/memory/memory_pool.h"

namespace mycache {

namespace {

std::atomic<size_t> g_used_memory{0};

// 系统实际分配给该指针的可用字节数（glibc 提供）。
inline size_t UsableSize(void* p) {
  return p ? ::malloc_usable_size(p) : 0;
}

void AdjustUsedMemory(long long delta) {
  if (delta == 0) return;
  if (delta > 0) {
    g_used_memory.fetch_add(static_cast<size_t>(delta), std::memory_order_relaxed);
  } else {
    g_used_memory.fetch_sub(static_cast<size_t>(-delta), std::memory_order_relaxed);
  }
}

}  // namespace

void* zmalloc(size_t size) {
  if (!PoolDisabled() && size > 0 && size <= kMaxPoolSize) {
    uint32_t cls = 0;
    void* p = PoolAlloc(size, &cls);
    if (p) g_used_memory.fetch_add(cls, std::memory_order_relaxed);  // 按 class 记账
    return p;
  }
  void* p = std::malloc(size);
  if (p) g_used_memory.fetch_add(UsableSize(p), std::memory_order_relaxed);
  return p;
}

void* zcalloc(size_t size) {
  // 有意绕过池：calloc 语义要清零，池块 pop 出来不保证零，直连更简单。
  void* p = std::calloc(1, size);
  if (p) g_used_memory.fetch_add(UsableSize(p), std::memory_order_relaxed);
  return p;
}

void* zrealloc(void* ptr, size_t size) {
  if (!ptr) return zmalloc(size);
  if (size == 0) { zfree(ptr); return nullptr; }

  if (PoolDisabled()) {
    // 池关闭：完全走原逻辑，保证 A/B 里「池关」口径与改动前一致。
    size_t old = UsableSize(ptr);
    void* p = std::realloc(ptr, size);
    if (p) AdjustUsedMemory(static_cast<long long>(UsableSize(p)) - static_cast<long long>(old));
    return p;
  }

  uint32_t old_cls = 0;
  bool pooled = PoolOwns(ptr, &old_cls);

  if (pooled && size <= old_cls) return ptr;  // 同 class 装得下：原地复用

  // 真实块且仍是大块（>4096）：保留原 in-place realloc，避免无谓拷贝。
  if (!pooled && size > kMaxPoolSize) {
    size_t old = UsableSize(ptr);
    void* p = std::realloc(ptr, size);
    if (p) AdjustUsedMemory(static_cast<long long>(UsableSize(p)) - static_cast<long long>(old));
    return p;
  }

  // 其余（池→更大池 / 池→大块 / 真实→池）：新块 + 拷贝 + 释放旧块。
  size_t old_sz = pooled ? static_cast<size_t>(old_cls) : UsableSize(ptr);
  void* np = zmalloc(size);  // size<=4096 走池，>4096 走直连
  if (np) {
    std::memcpy(np, ptr, old_sz < size ? old_sz : size);  // 拷贝上界，安全（可能略多拷）
    if (pooled) {
      zfree(ptr);  // 池块归还 freelist，used_memory -= old_cls
    } else {
      std::free(ptr);  // 真实块裸 free
      g_used_memory.fetch_sub(old_sz, std::memory_order_relaxed);
    }
  }
  return np;
}

void zfree(void* ptr) {
  if (!ptr) return;
  if (!PoolDisabled()) {
    uint32_t cls = 0;
    if (PoolOwns(ptr, &cls)) {
      g_used_memory.fetch_sub(cls, std::memory_order_relaxed);
      PoolFree(ptr);
      return;
    }
  }
  g_used_memory.fetch_sub(UsableSize(ptr), std::memory_order_relaxed);
  std::free(ptr);
}

size_t used_memory() {
  return g_used_memory.load(std::memory_order_relaxed);
}

}  // namespace mycache
