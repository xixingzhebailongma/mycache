#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "mycache/storage/object/encoding.h"

namespace mycache {

// 高层内存统计：按编码统计字符串对象数量。进程级单例，跨线程安全。
class MemoryStats {
 public:
  static MemoryStats& Instance();

  void IncrString(ObjEncoding enc);
  void DecrString(ObjEncoding enc);

  size_t StringCount(ObjEncoding enc) const;
  size_t TotalStrings() const;

  // 生成人类可读的统计摘要（类似 INFO memory 的精简版）。
  std::string Summary() const;

 private:
  MemoryStats() = default;

  std::atomic<size_t> int_count_{0};
  std::atomic<size_t> string_count_{0};
};

}  // namespace mycache
