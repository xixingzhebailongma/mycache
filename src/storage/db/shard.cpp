#include "mycache/storage/db/shard.h"

#include <cstdint>

#include "mycache/common/time.h"
#include "mycache/eviction/lru.h"
#include "mycache/eviction/sampler.h"
#include "mycache/monitor/metrics.h"
#include "mycache/storage/collection/hash.h"
#include "mycache/storage/collection/list.h"
#include "mycache/storage/db/dict_types.h"
#include "mycache/storage/memory/allocator.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

Shard::Shard() : dict_(SdsObjectDictType()), expires_(SdsRawDictType()) {
  rng_.seed(std::random_device{}());
}

Shard::~Shard() = default;

// ---- 内存治理 ----

void Shard::ConfigureEviction(const EvictionConfig& cfg) {
  eviction_ = cfg;
}

bool Shard::EvictIfNeeded() {
  if (eviction_.maxmemory <= 0) return true;  // 不限制

  const size_t limit = static_cast<size_t>(eviction_.maxmemory);
  if (eviction_.policy == EvictionPolicy::kNoEviction) {
    return used_memory() <= limit;  // 超限则拒绝写入
  }

  // 淘汰直到低于上限；设置迭代上限，避免极端情况下空转。
  int max_iter = 1000;
  while (used_memory() > limit && max_iter-- > 0) {
    if (!EvictOne()) break;  // 没有可淘汰的 key 了
  }
  return used_memory() <= limit;
}

bool Shard::EvictOne() {
  auto candidates = SampleCandidates();
  if (candidates.empty()) return false;
  sds victim = PickVictim(candidates);
  bool removed = DeleteKey(victim);
  if (removed) Metrics::Instance().IncrEvicted();
  return removed;
}

void Shard::IncrementalRehash(int ms) {
  dict_.RehashMilliseconds(ms);
  expires_.RehashMilliseconds(ms);
}

std::vector<DictEntry*> Shard::SampleCandidates() {
  size_t n = eviction_.maxmemory_samples > 0
                 ? static_cast<size_t>(eviction_.maxmemory_samples)
                 : 5;
  switch (eviction_.policy) {
    case EvictionPolicy::kAllkeysLru:
    case EvictionPolicy::kAllkeysRandom:
      return SampleDictEntries(&dict_, n, rng_);
    case EvictionPolicy::kVolatileLru:
    case EvictionPolicy::kVolatileRandom:
      return SampleDictEntries(&expires_, n, rng_);
    default:
      return {};
  }
}

sds Shard::PickVictim(const std::vector<DictEntry*>& candidates) {
  if (candidates.empty()) return nullptr;
  if (eviction_.policy == EvictionPolicy::kAllkeysLru ||
      eviction_.policy == EvictionPolicy::kVolatileLru) {
    // 找最久未访问（lru 最小）的 key。
    DictEntry* victim = candidates[0];
    uint64_t best = UINT64_MAX;
    for (auto* e : candidates) {
      Object* o = nullptr;
      if (eviction_.policy == EvictionPolicy::kAllkeysLru) {
        o = static_cast<Object*>(e->val);
      } else {
        // volatile：e 来自 expires_，val 是到期时间戳；去 dict_ 查对象。
        DictEntry* de = dict_.FindAndRehash(e->key);
        if (de) o = static_cast<Object*>(de->val);
      }
      uint64_t lru = o ? o->lru : 0;
      if (lru < best) {
        best = lru;
        victim = e;
      }
    }
    return static_cast<sds>(victim->key);
  }
  // random：候选集本身已是随机采样，再随机取一个。
  size_t idx = static_cast<size_t>(rng_() % candidates.size());
  return static_cast<sds>(candidates[idx]->key);
}

// ---- 内部工具 ----

bool Shard::DeleteKey(sds key) {
  bool existed = dict_.Delete(key);
  if (existed) key_count_.fetch_sub(1, std::memory_order_relaxed);
  if (expires_.Delete(key)) expire_count_.fetch_sub(1, std::memory_order_relaxed);
  return existed;
}

bool Shard::ExpireIfNeeded(sds key) {
  DictEntry* e = expires_.FindAndRehash(key);
  if (!e) return false;
  int64_t when = static_cast<int64_t>(reinterpret_cast<intptr_t>(e->val));
  if (UnixMs() < when) return false;
  DeleteKey(key);
  Metrics::Instance().IncrExpired();
  return true;
}

Object* Shard::LookupValue(sds key) {
  if (ExpireIfNeeded(key)) return nullptr;
  DictEntry* e = dict_.FindAndRehash(key);
  if (!e) return nullptr;
  Object* o = static_cast<Object*>(e->val);
  o->lru = LruClock();  // 访问即刷新 LRU
  return o;
}

// ---- string ----

bool Shard::Get(const std::string& key, std::string* out) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) {
    sdsfree(k);
    return false;
  }
  *out = o->ToString();
  sdsfree(k);
  return true;
}

void Shard::Set(const std::string& key, const std::string& value) {
  Object* o = CreateStringObject(value.data(), value.size());
  o = TryObjectEncoding(o);
  sds k = sdsnewlen(key.data(), key.size());
  if (expires_.Delete(k)) expire_count_.fetch_sub(1, std::memory_order_relaxed);  // SET 清除 TTL
  if (dict_.Replace(k, o) == 1) key_count_.fetch_add(1, std::memory_order_relaxed);
  sdsfree(k);
  o->DecrRefCount();  // 释放本函数持有的引用（dict 内部已 incref）
}

int Shard::Del(const std::vector<std::string>& keys) {
  int removed = 0;
  for (const auto& key : keys) {
    sds k = sdsnewlen(key.data(), key.size());
    if (DeleteKey(k)) ++removed;
    sdsfree(k);
  }
  return removed;
}

bool Shard::Exists(const std::string& key) {
  sds k = sdsnewlen(key.data(), key.size());
  bool found = LookupValue(k) != nullptr;
  sdsfree(k);
  return found;
}

std::string Shard::Type(const std::string& key) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  std::string t = "none";
  if (o) t = ObjTypeName(o->type);
  sdsfree(k);
  return t;
}

// ---- hash ----

int Shard::HSet(const std::string& key, const std::string& field, const std::string& value) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) {
    o = CreateHashObject();
    if (dict_.Add(k, o) == 1) key_count_.fetch_add(1, std::memory_order_relaxed);
    o->DecrRefCount();  // dict 已 incref，释放本地引用
  } else if (!o->IsHash()) {
    sdsfree(k);
    return -1;  // WRONGTYPE（命令层会先行检查，这里兜底）
  }

  Dict* hd = HashDict(o);
  Object* fv = CreateStringObject(value.data(), value.size());
  fv = TryObjectEncoding(fv);
  sds f = sdsnewlen(field.data(), field.size());
  int added = hd->Replace(f, fv);  // 1=新增字段，0=覆盖已有字段
  sdsfree(f);
  fv->DecrRefCount();
  sdsfree(k);
  return added;
}

bool Shard::HGet(const std::string& key, const std::string& field, std::string* out) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsHash()) {
    sdsfree(k);
    return false;
  }
  Dict* hd = HashDict(o);
  sds f = sdsnewlen(field.data(), field.size());
  DictEntry* e = hd->FindAndRehash(f);
  sdsfree(f);
  bool found = e != nullptr;
  if (found) *out = static_cast<Object*>(e->val)->ToString();
  sdsfree(k);
  return found;
}

int Shard::HDel(const std::string& key, const std::vector<std::string>& fields) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsHash()) {
    sdsfree(k);
    return 0;
  }
  Dict* hd = HashDict(o);
  int removed = 0;
  for (const auto& field : fields) {
    sds f = sdsnewlen(field.data(), field.size());
    if (hd->Delete(f)) ++removed;
    sdsfree(f);
  }
  // hash 清空后删除 key（与 Redis 一致）。
  if (hd->Size() == 0) DeleteKey(k);
  sdsfree(k);
  return removed;
}

std::vector<std::pair<std::string, std::string>> Shard::HGetAll(const std::string& key) {
  std::vector<std::pair<std::string, std::string>> result;
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsHash()) {
    sdsfree(k);
    return result;
  }
  Dict* hd = HashDict(o);
  // rehash 期间条目分布在两张表上，需同时遍历。
  for (int t = 0; t < 2; ++t) {
    const HashTable& ht = hd->Table(t);
    if (!ht.table) continue;
    for (size_t i = 0; i < ht.size; ++i) {
      for (DictEntry* he = ht.table[i]; he; he = he->next) {
        sds f = static_cast<sds>(he->key);
        Object* v = static_cast<Object*>(he->val);
        result.emplace_back(std::string(f, sdslen(f)), v->ToString());
      }
    }
  }
  sdsfree(k);
  return result;
}

// ---- list ----

int Shard::LPush(const std::string& key, const std::vector<std::string>& values) {
  if (values.empty()) return -1;  // arity 保证至少一个值
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) {
    o = CreateListObject();
    if (dict_.Add(k, o) == 1) key_count_.fetch_add(1, std::memory_order_relaxed);
    o->DecrRefCount();
  } else if (!o->IsList()) {
    sdsfree(k);
    return -1;
  }
  List* l = ListPtr(o);
  for (const auto& v : values) {
    ListPushFront(l, sdsnewlen(v.data(), v.size()));
  }
  size_t len = l->len;
  sdsfree(k);
  return static_cast<int>(len);
}

int Shard::RPush(const std::string& key, const std::vector<std::string>& values) {
  if (values.empty()) return -1;
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) {
    o = CreateListObject();
    if (dict_.Add(k, o) == 1) key_count_.fetch_add(1, std::memory_order_relaxed);
    o->DecrRefCount();
  } else if (!o->IsList()) {
    sdsfree(k);
    return -1;
  }
  List* l = ListPtr(o);
  for (const auto& v : values) {
    ListPushBack(l, sdsnewlen(v.data(), v.size()));
  }
  size_t len = l->len;
  sdsfree(k);
  return static_cast<int>(len);
}

bool Shard::LPop(const std::string& key, std::string* out) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsList()) {
    sdsfree(k);
    return false;
  }
  List* l = ListPtr(o);
  sds v = ListPopFront(l);
  if (!v) {
    sdsfree(k);
    return false;
  }
  *out = std::string(v, sdslen(v));
  sdsfree(v);
  if (l->len == 0) DeleteKey(k);  // 空列表删除 key
  sdsfree(k);
  return true;
}

bool Shard::RPop(const std::string& key, std::string* out) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsList()) {
    sdsfree(k);
    return false;
  }
  List* l = ListPtr(o);
  sds v = ListPopBack(l);
  if (!v) {
    sdsfree(k);
    return false;
  }
  *out = std::string(v, sdslen(v));
  sdsfree(v);
  if (l->len == 0) DeleteKey(k);
  sdsfree(k);
  return true;
}

std::vector<std::string> Shard::LRange(const std::string& key, long start, long stop) {
  std::vector<std::string> out;
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsList()) {
    sdsfree(k);
    return out;
  }
  List* l = ListPtr(o);
  long len = static_cast<long>(l->len);
  if (len == 0) {
    sdsfree(k);
    return out;
  }
  // 归一化负下标并夹到 [0, len-1]。
  if (start < 0) start = len + start;
  if (stop < 0) stop = len + stop;
  if (start < 0) start = 0;
  if (stop >= len) stop = len - 1;
  if (start > stop || start >= len) {
    sdsfree(k);
    return out;
  }
  ListNode* n = l->head;
  for (long i = 0; i < start && n; ++i) n = n->next;
  for (long i = start; i <= stop && n; ++i, n = n->next) {
    out.emplace_back(n->value, sdslen(n->value));
  }
  sdsfree(k);
  return out;
}

size_t Shard::LLen(const std::string& key) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o || !o->IsList()) {
    sdsfree(k);
    return 0;
  }
  size_t len = ListPtr(o)->len;
  sdsfree(k);
  return len;
}

// ---- TTL ----

bool Shard::ExpireAt(const std::string& key, int64_t when_ms) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) {
    sdsfree(k);
    return false;
  }
  if (expires_.Replace(k, reinterpret_cast<void*>(static_cast<intptr_t>(when_ms))) == 1) {
    expire_count_.fetch_add(1, std::memory_order_relaxed);
  }
  sdsfree(k);
  return true;
}

bool Shard::ExpireMs(const std::string& key, int64_t ms) {
  return ExpireAt(key, UnixMs() + ms);
}

int64_t Shard::TtlMs(const std::string& key) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);  // 触发惰性删除
  if (!o) {
    sdsfree(k);
    return -2;
  }
  DictEntry* e = expires_.FindAndRehash(k);
  if (!e) {
    sdsfree(k);
    return -1;
  }
  int64_t when = static_cast<int64_t>(reinterpret_cast<intptr_t>(e->val));
  int64_t remain = when - UnixMs();
  if (remain < 0) remain = 0;
  sdsfree(k);
  return remain;
}

void Shard::ActiveExpireCycle() {
  constexpr int kSamples = 20;
  if (expires_.Size() == 0) return;

  for (int i = 0; i < kSamples; ++i) {
    size_t size0 = expires_.Table(0).size;
    size_t size1 = expires_.Table(1).size;
    size_t total = size0 + size1;
    if (total == 0) break;

    size_t idx = static_cast<size_t>(rng_() % total);
    DictEntry* head = (idx < size0)
                          ? (expires_.Table(0).table ? expires_.Table(0).table[idx] : nullptr)
                          : (expires_.Table(1).table ? expires_.Table(1).table[idx - size0] : nullptr);

    // 先收集本桶链中已到期的 key（避免在遍历中删除导致迭代器失效）。
    std::vector<std::string> expired;
    for (DictEntry* he = head; he; he = he->next) {
      int64_t when = static_cast<int64_t>(reinterpret_cast<intptr_t>(he->val));
      if (UnixMs() >= when) {
        sds k = static_cast<sds>(he->key);
        expired.emplace_back(k, sdslen(k));
      }
    }
    for (const auto& key : expired) {
      sds k = sdsnewlen(key.data(), key.size());
      if (DeleteKey(k)) Metrics::Instance().IncrExpired();
      sdsfree(k);
    }
  }
}

size_t Shard::Size() const {
  return key_count_.load(std::memory_order_relaxed);
}

size_t Shard::ExpireCount() const {
  return expire_count_.load(std::memory_order_relaxed);
}

}  // namespace mycache
