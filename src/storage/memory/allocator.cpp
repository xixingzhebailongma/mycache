#include "mycache/storage/memory/allocator.h"

#include <atomic>
#include <cstdlib>

#include <malloc.h>

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
  void* p = std::malloc(size);
  if (p) g_used_memory.fetch_add(UsableSize(p), std::memory_order_relaxed);
  return p;
}

void* zcalloc(size_t size) {
  void* p = std::calloc(1, size);
  if (p) g_used_memory.fetch_add(UsableSize(p), std::memory_order_relaxed);
  return p;
}

void* zrealloc(void* ptr, size_t size) {
  size_t old = UsableSize(ptr);
  void* p = std::realloc(ptr, size);
  if (p) AdjustUsedMemory(static_cast<long long>(UsableSize(p)) - static_cast<long long>(old));
  return p;
}

void zfree(void* ptr) {
  if (!ptr) return;
  g_used_memory.fetch_sub(UsableSize(ptr), std::memory_order_relaxed);
  std::free(ptr);
}

size_t used_memory() {
  return g_used_memory.load(std::memory_order_relaxed);
}

}  // namespace mycache
