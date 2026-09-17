#include "mycache/storage/dict/dict.h"

#include <cstring>

#include "mycache/storage/memory/allocator.h"

namespace mycache {

uint64_t DictHash(const void* key, size_t len) {
  // MurmurHash64A：对任意字节序列做 64 位哈希，分布良好且实现紧凑。
  // 通过 memcpy 以 8 字节块读取，避免未对齐访问（sds 数据可能非 8 字节对齐）。
  const uint64_t m = 0xc6a4a7935bd1e995ULL;
  const int r = 47;
  uint64_t h = len * m;

  const unsigned char* data = static_cast<const unsigned char*>(key);
  const size_t nblocks = len / 8;
  for (size_t i = 0; i < nblocks; ++i) {
    uint64_t k;
    std::memcpy(&k, data + i * 8, sizeof(k));
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }

  const unsigned char* tail = data + nblocks * 8;
  switch (len & 7) {
    case 7: h ^= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
    case 6: h ^= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
    case 5: h ^= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
    case 4: h ^= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
    case 3: h ^= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
    case 2: h ^= static_cast<uint64_t>(tail[1]) << 8; [[fallthrough]];
    case 1: h ^= static_cast<uint64_t>(tail[0]); h *= m; [[fallthrough]];
    default: break;
  }

  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

Dict::Dict(DictType* type) : type_(type) {}

Dict::~Dict() { Clear(); }

size_t Dict::NextPower(size_t size) const {
  size_t i = kDictInitialSize;
  while (i < size) i <<= 1;
  return i;
}

uint64_t Dict::KeyHash(const void* key) const { return type_->hash(key); }

bool Dict::KeyEqual(const void* a, const void* b) const {
  if (a == b) return true;
  return type_->equal(a, b);
}

DictEntry* Dict::AllocEntry() {
  return static_cast<DictEntry*>(zmalloc(sizeof(DictEntry)));
}

void Dict::SetKey(DictEntry* e, const void* key) {
  e->key = type_->key_dup ? type_->key_dup(key) : const_cast<void*>(key);
}

void Dict::SetVal(DictEntry* e, const void* val) {
  e->val = type_->val_dup ? type_->val_dup(val) : const_cast<void*>(val);
}

void Dict::FreeKey(DictEntry* e) {
  if (type_->key_destructor && e->key) type_->key_destructor(e->key);
}

void Dict::FreeVal(DictEntry* e) {
  if (type_->val_destructor && e->val) type_->val_destructor(e->val);
}

void Dict::FreeEntry(DictEntry* e) {
  FreeKey(e);
  FreeVal(e);
  zfree(e);
}

DictEntry* Dict::AddRaw(const void* key, DictEntry** existing) {
  if (existing) *existing = nullptr;
  RehashStep();                        // 写路径推进进行中的 rehash
  if (ExpandIfNeeded() != 0) return nullptr;  // 必要时扩容（可能开始 rehash）

  uint64_t h = KeyHash(key);
  size_t idx = 0;
  // 查找键是否已存在；rehash 中需查两张表。
  for (int t = 0; t <= 1; ++t) {
    idx = h & ht_[t].sizemask;
    for (DictEntry* he = ht_[t].table[idx]; he; he = he->next) {
      if (KeyEqual(key, he->key)) {
        if (existing) *existing = he;
        return nullptr;
      }
    }
    if (!Rehashing()) break;
  }

  // rehash 中写入 ht[1]，否则写入 ht[0]。
  HashTable& ht = Rehashing() ? ht_[1] : ht_[0];
  idx = h & ht.sizemask;

  DictEntry* e = AllocEntry();
  if (!e) return nullptr;
  SetKey(e, key);
  e->next = ht.table[idx];
  ht.table[idx] = e;
  ht.used++;
  return e;
}

int Dict::Add(const void* key, const void* val) {
  DictEntry* existing = nullptr;
  DictEntry* e = AddRaw(key, &existing);
  if (!e) return 0;  // 键已存在
  SetVal(e, val);
  return 1;
}

int Dict::Replace(const void* key, const void* val) {
  DictEntry* existing = nullptr;
  DictEntry* e = AddRaw(key, &existing);
  if (e) {
    SetVal(e, val);
    return 1;
  }
  // 键已存在：先释放旧值再设置新值（value 引用计数因此保持正确）。
  DictEntry aux = *existing;
  SetVal(existing, val);
  FreeVal(&aux);
  return 0;
}

DictEntry* Dict::Find(const void* key) const {
  if (Size() == 0) return nullptr;
  uint64_t h = KeyHash(key);
  for (int t = 0; t <= 1; ++t) {
    size_t idx = h & ht_[t].sizemask;
    for (DictEntry* he = ht_[t].table[idx]; he; he = he->next) {
      if (KeyEqual(key, he->key)) return he;
    }
    if (!Rehashing()) break;
  }
  return nullptr;
}

DictEntry* Dict::FindAndRehash(const void* key) {
  if (Size() == 0) return nullptr;
  RehashStep();  // 读路径也推进 rehash（对齐 Redis dictFind）
  uint64_t h = KeyHash(key);
  for (int t = 0; t <= 1; ++t) {
    size_t idx = h & ht_[t].sizemask;
    for (DictEntry* he = ht_[t].table[idx]; he; he = he->next) {
      if (KeyEqual(key, he->key)) return he;
    }
    if (!Rehashing()) break;
  }
  return nullptr;
}

void* Dict::FetchValue(const void* key) const {
  DictEntry* e = Find(key);
  return e ? e->val : nullptr;
}

bool Dict::Delete(const void* key) {
  if (Size() == 0) return false;
  RehashStep();
  uint64_t h = KeyHash(key);
  for (int t = 0; t <= 1; ++t) {
    size_t idx = h & ht_[t].sizemask;
    DictEntry* he = ht_[t].table[idx];
    DictEntry* prev = nullptr;
    while (he) {
      if (KeyEqual(key, he->key)) {
        if (prev) prev->next = he->next;
        else ht_[t].table[idx] = he->next;
        FreeEntry(he);
        ht_[t].used--;
        ShrinkIfNeeded();
        return true;
      }
      prev = he;
      he = he->next;
    }
    if (!Rehashing()) break;
  }
  return false;
}

void Dict::Clear() {
  for (int t = 0; t < 2; ++t) {
    if (ht_[t].table) {
      for (size_t i = 0; i < ht_[t].size; ++i) {
        DictEntry* he = ht_[t].table[i];
        while (he) {
          DictEntry* next = he->next;
          FreeEntry(he);
          he = next;
        }
      }
      zfree(ht_[t].table);
      ht_[t] = HashTable{};
    }
  }
  rehashidx_ = -1;
}

}  // namespace mycache
