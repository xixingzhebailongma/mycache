#include "mycache/eviction/sampler.h"

#include "mycache/storage/dict/dict.h"

namespace mycache {

std::vector<DictEntry*> SampleDictEntries(Dict* d, size_t count, std::mt19937_64& rng) {
  std::vector<DictEntry*> out;
  out.reserve(count);
  if (!d || count == 0 || d->Size() == 0) return out;

  // 两张表的总桶数（rehash 中条目分布在 ht[0] 与 ht[1]）。
  size_t size0 = d->Table(0).size;
  size_t size1 = d->Rehashing() ? d->Table(1).size : 0;
  size_t total = size0 + size1;
  if (total == 0) return out;

  // 最多尝试 count*10 次随机选桶，避免在稀疏表里空转太久。
  size_t max_steps = count * 10;
  for (size_t step = 0; step < max_steps && out.size() < count; ++step) {
    size_t idx = static_cast<size_t>(rng() % total);
    DictEntry* head = nullptr;
    if (idx < size0) {
      head = d->Table(0).table ? d->Table(0).table[idx] : nullptr;
    } else {
      head = d->Table(1).table ? d->Table(1).table[idx - size0] : nullptr;
    }
    for (DictEntry* he = head; he && out.size() < count; he = he->next) {
      out.push_back(he);
    }
  }
  return out;
}

}  // namespace mycache
