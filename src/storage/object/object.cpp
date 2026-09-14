#include "mycache/storage/object/object.h"

#include <string>

#include "mycache/storage/collection/hash.h"
#include "mycache/storage/collection/list.h"
#include "mycache/storage/memory/allocator.h"
#include "mycache/storage/memory/memory_stats.h"
#include "mycache/storage/object/string_object.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

void Object::DecrRefCount() {
  if (--refcount > 0) return;
  switch (type) {
    case ObjType::kString:
      MemoryStats::Instance().DecrString(encoding);
      // 独立分配的 sds，需单独释放（int 编码无内部分配）。
      if (encoding == ObjEncoding::kString) sdsfree(static_cast<sds>(ptr));
      break;
    case ObjType::kHash:
      ReleaseHashObject(this);  // 释放内部 dict（连同其中所有字段值）
      break;
    case ObjType::kList:
      ReleaseListObject(this);  // 释放链表与节点
      break;
    default:
      break;
  }
  zfree(this);
}

std::string Object::ToString() const {
  if (encoding == ObjEncoding::kInt) {
    return std::to_string(ObjectPtrToLongLong(ptr));
  }
  sds s = static_cast<sds>(ptr);
  return std::string(s, sdslen(s));
}

bool Object::TryGetLongLong(long long* out) const {
  if (encoding == ObjEncoding::kInt) {
    *out = ObjectPtrToLongLong(ptr);
    return true;
  }
  sds s = static_cast<sds>(ptr);
  return StringToLongLong(s, sdslen(s), out);
}

}  // namespace mycache
