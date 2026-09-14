#pragma once

#include <cstdint>

namespace mycache {

// 对象类型。Week 2 仅实现字符串；其余为后续周次预留。
enum class ObjType : uint8_t {
  kString = 0,
  kList,
  kHash,
  kSet,
  kZSet,
};

// 对象编码。Week 2 对字符串实现 kInt / kString（字符串）两种编码。
enum class ObjEncoding : uint8_t {
  kString = 0,  // 字符串：内容存放在独立分配的 sds 中
  kInt,         // 整数：long long 直接嵌在 ptr 字段
  // 后续周次的集合编码
  kHt,        // hash table（dict）
  kListpack,
  kIntSet,
  kSkipList,
  kQuickList,
  kLinkedList,  // Week 5: 双向链表（list 类型）
};

inline const char* ObjTypeName(ObjType t) {
  switch (t) {
    case ObjType::kString: return "string";
    case ObjType::kList: return "list";
    case ObjType::kHash: return "hash";
    case ObjType::kSet: return "set";
    case ObjType::kZSet: return "zset";
  }
  return "unknown";
}

inline const char* ObjEncodingName(ObjEncoding e) {
  switch (e) {
    case ObjEncoding::kString: return "string";
    case ObjEncoding::kInt: return "int";
    case ObjEncoding::kHt: return "hashtable";
    case ObjEncoding::kListpack: return "listpack";
    case ObjEncoding::kIntSet: return "intset";
    case ObjEncoding::kSkipList: return "skiplist";
    case ObjEncoding::kQuickList: return "quicklist";
    case ObjEncoding::kLinkedList: return "linkedlist";
  }
  return "unknown";
}

}  // namespace mycache
