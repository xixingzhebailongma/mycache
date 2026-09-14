#pragma once

#include "mycache/storage/dict/dict.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

// 共享的 dict 类型工厂：sds 键 + 不同种类的值。集中定义以避免在
// Keyspace / Shard / hash 之间重复。

inline uint64_t SdsKeyHash(const void* key) {
  return DictHash(key, sdslen(static_cast<const char*>(key)));
}

inline bool SdsKeyEqual(const void* a, const void* b) {
  return sdscmp(static_cast<const char*>(a), static_cast<const char*>(b)) == 0;
}

inline void* SdsKeyDup(const void* key) {
  return sdsdup(static_cast<const char*>(key));
}

inline void SdsKeyFree(void* key) {
  sdsfree(static_cast<sds>(key));
}

inline void* ObjectDup(const void* val) {
  auto* o = const_cast<Object*>(static_cast<const Object*>(val));
  o->IncrRefCount();
  return o;
}

inline void ObjectFree(void* val) {
  static_cast<Object*>(val)->DecrRefCount();
}

// sds key -> Object*（引用计数）的 dict 类型，用于主键空间与 hash 字段表。
inline DictType* SdsObjectDictType() {
  static DictType t;
  t.hash = SdsKeyHash;
  t.equal = SdsKeyEqual;
  t.key_dup = SdsKeyDup;
  t.key_destructor = SdsKeyFree;
  t.val_dup = ObjectDup;
  t.val_destructor = ObjectFree;
  return &t;
}

// sds key -> 原始 void*（如 int64_t 到期时间戳）的 dict 类型。
inline DictType* SdsRawDictType() {
  static DictType t;
  t.hash = SdsKeyHash;
  t.equal = SdsKeyEqual;
  t.key_dup = SdsKeyDup;
  t.key_destructor = SdsKeyFree;
  t.val_dup = nullptr;
  t.val_destructor = nullptr;
  return &t;
}

}  // namespace mycache
