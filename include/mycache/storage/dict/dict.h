#pragma once

#include <cstddef>
#include <cstdint>

#include "mycache/storage/dict/hash_table.h"

namespace mycache {

// 哈希表中的一个条目：键 + 值 + 下一个指针（单独链地址法）。
struct DictEntry {
  void* key = nullptr;
  void* val = nullptr;
  DictEntry* next = nullptr;
};

// dict 的类型参数：由使用者提供针对键/值的操作。
// `equal` 返回 true 表示键相等（与 Redis 的 dictCompareKeys 约定相反，这里直接
// 用布尔值以贴合 C++ 习惯）。
struct DictType {
  uint64_t (*hash)(const void* key) = nullptr;
  bool (*equal)(const void* a, const void* b) = nullptr;
  void* (*key_dup)(const void* key) = nullptr;         // 可为 nullptr
  void (*key_destructor)(void* key) = nullptr;          // 可为 nullptr
  void* (*val_dup)(const void* val) = nullptr;          // 可为 nullptr
  void (*val_destructor)(void* val) = nullptr;          // 可为 nullptr
};

// 初始 / 最小桶数（2 的幂）。
constexpr size_t kDictInitialSize = 4;

// 对任意字节序列做 64 位哈希（MurmurHash64A）。SDS 键的 hash 函数基于它实现。
uint64_t DictHash(const void* data, size_t len);

// Redis 风格的字典：单独链地址 + 双哈希表 + 渐进式 rehash。
//
// - 查询（Find）是只读的，不会推进 rehash；rehash 仅在写路径（Add/Delete/Replace）
//   通过 RehashStep 推进。这样上层可以用共享锁保护读、独占锁保护写。
// - 扩容/收缩通过 Expand 触发，分配 ht[1] 并把 rehashidx 置 0；之后每次写操作
//   迁移少量桶，直到 ht[0] 清空后交换两表。
//
// 非线程安全；并发保护由上层（Keyspace）提供。
class Dict {
 public:
  explicit Dict(DictType* type);
  ~Dict();
  Dict(const Dict&) = delete;
  Dict& operator=(const Dict&) = delete;

  // ---- 查询 ----
  DictEntry* Find(const void* key) const;   // 只读，不推进 rehash
  void* FetchValue(const void* key) const;

  // ---- 写入 ----
  int Add(const void* key, const void* val);      // 键已存在则返回 0 且不改动
  int Replace(const void* key, const void* val);  // 已存在则覆盖旧值（先释放旧值）

  bool Delete(const void* key);

  // ---- resize / rehash ----
  int Expand(size_t size);          // 扩容/收缩到 size 的下一 2 次幂
  int Resize();                     // 收缩到 used 的最小 2 次幂
  int Rehash(int n);                // 迁移最多 n 个非空桶；返回 0 表示完成
  int RehashMilliseconds(int ms);   // 限时 rehash
  bool RehashStep();                // 单步（1 个桶），供写路径调用

  void Clear();

  size_t Size() const { return ht_[0].used + ht_[1].used; }
  bool Rehashing() const { return rehashidx_ != -1; }
  long RehashIndex() const { return rehashidx_; }
  const HashTable& Table(int i) const { return ht_[i]; }

 private:
  DictEntry* AddRaw(const void* key, DictEntry** existing);
  uint64_t KeyHash(const void* key) const;
  bool KeyEqual(const void* a, const void* b) const;
  void SetKey(DictEntry* e, const void* key);
  void SetVal(DictEntry* e, const void* val);
  void FreeKey(DictEntry* e);
  void FreeVal(DictEntry* e);
  void FreeEntry(DictEntry* e);
  DictEntry* AllocEntry();
  int ExpandIfNeeded();
  int ShrinkIfNeeded();
  size_t NextPower(size_t size) const;

  DictType* type_;
  HashTable ht_[2];
  long rehashidx_ = -1;  // -1 = 未 rehash；否则为 ht[0] 下一个待迁移桶下标
};

}  // namespace mycache
