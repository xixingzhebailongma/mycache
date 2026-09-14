#include "mycache/storage/db/keyspace.h"

#include <mutex>

#include "mycache/storage/memory/allocator.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/object/string_object.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

namespace {

// sds 键 + Object* 值的 dict 类型：键按值比较/复制，值按引用计数管理。
uint64_t SdsHash(const void* key) {
  return DictHash(key, sdslen(static_cast<const char*>(key)));
}

bool SdsEqual(const void* a, const void* b) {
  return sdscmp(static_cast<const char*>(a), static_cast<const char*>(b)) == 0;
}

void* SdsDup(const void* key) {
  return sdsdup(static_cast<const char*>(key));
}

void SdsFree(void* key) {
  sdsfree(static_cast<sds>(key));
}

void* ObjectDup(const void* val) {
  auto* o = const_cast<Object*>(static_cast<const Object*>(val));
  o->IncrRefCount();
  return o;
}

void ObjectFree(void* val) {
  static_cast<Object*>(val)->DecrRefCount();
}

DictType* SdsObjectDictType() {
  static DictType t;
  t.hash = SdsHash;
  t.equal = SdsEqual;
  t.key_dup = SdsDup;
  t.key_destructor = SdsFree;
  t.val_dup = ObjectDup;
  t.val_destructor = ObjectFree;
  return &t;
}

}  // namespace

Keyspace::Keyspace() : dict_(SdsObjectDictType()) {}

Keyspace::~Keyspace() = default;

bool Keyspace::Get(const std::string& key, std::string* out) {
  sds k = sdsnewlen(key.data(), key.size());
  std::shared_lock<std::shared_mutex> lock(mu_);
  DictEntry* e = dict_.Find(k);
  if (!e) {
    sdsfree(k);
    return false;
  }
  *out = static_cast<Object*>(e->val)->ToString();
  sdsfree(k);
  return true;
}

void Keyspace::Set(const std::string& key, const std::string& value) {
  // 对象创建与整数编码在锁外完成，缩短临界区。
  Object* o = CreateStringObject(value.data(), value.size());
  o = TryObjectEncoding(o);
  sds k = sdsnewlen(key.data(), key.size());
  {
    std::unique_lock<std::shared_mutex> lock(mu_);
    dict_.Replace(k, o);
  }
  sdsfree(k);
  o->DecrRefCount();  // 释放本函数持有的引用（dict 内部已 incref）
}

int Keyspace::Del(const std::vector<std::string>& keys) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  int removed = 0;
  for (const auto& key : keys) {
    sds k = sdsnewlen(key.data(), key.size());
    if (dict_.Delete(k)) ++removed;
    sdsfree(k);
  }
  return removed;
}

bool Keyspace::Exists(const std::string& key) {
  sds k = sdsnewlen(key.data(), key.size());
  std::shared_lock<std::shared_mutex> lock(mu_);
  bool found = dict_.Find(k) != nullptr;
  sdsfree(k);
  return found;
}

size_t Keyspace::Size() {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return dict_.Size();
}

size_t Keyspace::UsedMemory() const {
  return used_memory();
}

size_t Keyspace::BucketCount() {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return dict_.Table(0).size + dict_.Table(1).size;
}

bool Keyspace::Rehashing() {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return dict_.Rehashing();
}

}  // namespace mycache
