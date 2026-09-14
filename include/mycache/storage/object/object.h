#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mycache/storage/object/encoding.h"

namespace mycache {

// 指针 <-> long long 的互转（用于 kInt 编码：值直接存放在 ptr 字段）。
inline long long ObjectPtrToLongLong(void* p) {
  return static_cast<long long>(reinterpret_cast<intptr_t>(p));
}
inline void* LongLongToObjectPtr(long long v) {
  return reinterpret_cast<void*>(static_cast<intptr_t>(v));
}

// Redis 风格的引用计数对象（robj）。
//
// 对字符串对象（type == kString）支持两种编码：
//   kInt    — 值作为 long long 直接存放在 ptr 字段，不单独分配。
//   kString — 内容存放在独立分配的 sds 中。
struct Object {
  ObjType type;
  ObjEncoding encoding;
  int refcount;
  void* ptr;
  uint64_t lru = 0;  // 近似 LRU 时钟（LruClock()，毫秒）；创建时置，访问时刷新

  bool IsString() const { return type == ObjType::kString; }
  bool IsHash() const { return type == ObjType::kHash; }
  bool IsList() const { return type == ObjType::kList; }
  void IncrRefCount() { ++refcount; }

  // 引用计数减一；归零时释放整个对象（含独立分配的内部缓冲）。
  void DecrRefCount();

  // 拷贝出字符串内容；int 编码会格式化为十进制。
  std::string ToString() const;

  // 尝试严格解析为 long long（int 编码直接返回；字符串编码做严格解析）。
  bool TryGetLongLong(long long* out) const;

};

}  // namespace mycache
