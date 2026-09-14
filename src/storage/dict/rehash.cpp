#include "mycache/storage/dict/dict.h"

#include <chrono>

#include "mycache/storage/dict/rehash.h"
#include "mycache/storage/memory/allocator.h"

namespace mycache {

int Dict::Expand(size_t size) {
  if (Rehashing() || ht_[0].used > size) return -1;
  size_t realsize = NextPower(size);
  if (realsize == ht_[0].size) return -1;

  HashTable n;
  n.size = realsize;
  n.sizemask = realsize - 1;
  n.table = static_cast<DictEntry**>(zcalloc(realsize * sizeof(DictEntry*)));
  if (!n.table) return -1;  // OOM
  n.used = 0;

  if (ht_[0].table == nullptr) {
    ht_[0] = n;
    return 0;
  }
  // 分配 ht[1] 并启动渐进式 rehash。
  ht_[1] = n;
  rehashidx_ = 0;
  return 0;
}

int Dict::Resize() {
  size_t minimal = ht_[0].used;
  if (minimal < kDictInitialSize) minimal = kDictInitialSize;
  return Expand(minimal);
}

int Dict::ExpandIfNeeded() {
  if (Rehashing()) return 0;
  if (ht_[0].size == 0) return Expand(kDictInitialSize);
  if (ht_[0].used >= ht_[0].size) return Expand(ht_[0].used * 2);
  return 0;
}

int Dict::ShrinkIfNeeded() {
  if (Rehashing()) return 0;
  // 负载因子 < 1/8 且大于初始大小才收缩。
  if (ht_[0].size > kDictInitialSize && ht_[0].used * 8 < ht_[0].size) {
    return Resize();
  }
  return 0;
}

int Dict::Rehash(int n) {
  int empty_visits = n * kRehashEmptyVisitFactor;
  if (!Rehashing()) return 0;

  while (n-- && ht_[0].used != 0) {
    // 找到下一个非空桶。
    while (rehashidx_ < static_cast<long>(ht_[0].size) && ht_[0].table[rehashidx_] == nullptr) {
      rehashidx_++;
      if (--empty_visits == 0) return 1;
    }
    if (rehashidx_ >= static_cast<long>(ht_[0].size)) break;

    // 迁移该桶的整条链到 ht[1]。
    DictEntry* de = ht_[0].table[rehashidx_];
    while (de) {
      DictEntry* next = de->next;
      size_t idx = KeyHash(de->key) & ht_[1].sizemask;
      de->next = ht_[1].table[idx];
      ht_[1].table[idx] = de;
      ht_[0].used--;
      ht_[1].used++;
      de = next;
    }
    ht_[0].table[rehashidx_] = nullptr;
    rehashidx_++;
  }

  // 完成：释放 ht[0] 并交换两表。
  if (ht_[0].used == 0) {
    zfree(ht_[0].table);
    ht_[0] = ht_[1];
    ht_[1] = HashTable{};
    rehashidx_ = -1;
    return 0;
  }
  return 1;
}

int Dict::RehashMilliseconds(int ms) {
  using namespace std::chrono;
  auto start = steady_clock::now();
  int rehashes = 0;
  while (Rehash(100) != 0) {
    rehashes += 100;
    if (duration_cast<milliseconds>(steady_clock::now() - start).count() >= ms) break;
  }
  return rehashes;
}

bool Dict::RehashStep() {
  return Rehash(kRehashBucketPerStep) != 0;
}

}  // namespace mycache
