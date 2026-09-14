#include "mycache/storage/memory/memory_stats.h"

#include <cstdio>

#include "mycache/storage/memory/allocator.h"

namespace mycache {

MemoryStats& MemoryStats::Instance() {
  static MemoryStats inst;
  return inst;
}

void MemoryStats::IncrString(ObjEncoding enc) {
  switch (enc) {
    case ObjEncoding::kInt: int_count_.fetch_add(1, std::memory_order_relaxed); break;
    case ObjEncoding::kString: string_count_.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
  }
}

void MemoryStats::DecrString(ObjEncoding enc) {
  switch (enc) {
    case ObjEncoding::kInt: int_count_.fetch_sub(1, std::memory_order_relaxed); break;
    case ObjEncoding::kString: string_count_.fetch_sub(1, std::memory_order_relaxed); break;
    default: break;
  }
}

size_t MemoryStats::StringCount(ObjEncoding enc) const {
  switch (enc) {
    case ObjEncoding::kInt: return int_count_.load(std::memory_order_relaxed);
    case ObjEncoding::kString: return string_count_.load(std::memory_order_relaxed);
    default: return 0;
  }
}

size_t MemoryStats::TotalStrings() const {
  return int_count_.load(std::memory_order_relaxed) +
         string_count_.load(std::memory_order_relaxed);
}

std::string MemoryStats::Summary() const {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "used_memory:   %zu\n"
                "string objects:%zu (int: %zu, string: %zu)\n",
                used_memory(), TotalStrings(),
                int_count_.load(std::memory_order_relaxed),
                string_count_.load(std::memory_order_relaxed));
  return std::string(buf);
}

}  // namespace mycache
