#pragma once

#include <cstddef>

namespace mycache {

struct DictEntry;

// 单张哈希表：桶数组 + 元数据。dict 持有两张（ht[0] 用于服务请求，ht[1] 用于
// 渐进式 rehash 时的目标表）。
struct HashTable {
  DictEntry** table = nullptr;  // 桶数组，每个桶是一条 DictEntry 链
  size_t size = 0;              // 桶数量（2 的幂）
  size_t sizemask = 0;          // size - 1，用于 idx = hash & sizemask
  size_t used = 0;              // 当前条目数
};

}  // namespace mycache
