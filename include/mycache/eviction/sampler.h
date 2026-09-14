#pragma once

#include <cstddef>
#include <random>
#include <vector>

namespace mycache {

struct Dict;
struct DictEntry;

// 从 dict 中随机采样最多 `count` 个条目，返回采到的 DictEntry*（键为 sds）。
//
// 近似采样：随机挑桶、沿链收集，rehash 期间两张表都参与。与 Redis 的
// dictGetSomeKeys 类似，采样结果不必严格均匀，够近似 LRU / random 淘汰用。
// 表很小或连续命中空桶时返回的条目数可能少于 count。
std::vector<DictEntry*> SampleDictEntries(Dict* d, size_t count, std::mt19937_64& rng);

}  // namespace mycache
