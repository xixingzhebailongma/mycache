#include <string>

#include <gtest/gtest.h>

#include "mycache/storage/dict/dict.h"
#include "mycache/storage/string/sds.h"

using mycache::Dict;
using mycache::DictEntry;
using mycache::DictType;
using mycache::sds;

namespace {

uint64_t SdsHash(const void* key) {
  return mycache::DictHash(key, mycache::sdslen(static_cast<const char*>(key)));
}
bool SdsEq(const void* a, const void* b) {
  return mycache::sdscmp(static_cast<const char*>(a), static_cast<const char*>(b)) == 0;
}
void* SdsDup(const void* key) { return mycache::sdsdup(static_cast<const char*>(key)); }
void SdsFree(void* key) { mycache::sdsfree(static_cast<sds>(key)); }

DictType* TestType() {
  static DictType t;
  t.hash = SdsHash;
  t.equal = SdsEq;
  t.key_dup = SdsDup;
  t.key_destructor = SdsFree;
  t.val_dup = nullptr;
  t.val_destructor = nullptr;
  return &t;
}

// 便捷封装：std::string 键 + long 值（值存于 void* 中，无析构）。
struct TestDict {
  Dict d{TestType()};

  sds NewKey(const std::string& k) { return mycache::sdsnewlen(k.data(), k.size()); }

  int Set(const std::string& k, long v) {
    sds key = NewKey(k);
    int r = d.Add(key, reinterpret_cast<void*>(v));
    mycache::sdsfree(key);
    return r;
  }
  int Replace(const std::string& k, long v) {
    sds key = NewKey(k);
    int r = d.Replace(key, reinterpret_cast<void*>(v));
    mycache::sdsfree(key);
    return r;
  }
  long Get(const std::string& k) {
    sds key = NewKey(k);
    // 用条目指针判断是否存在：值可能为 0（即 null 的 void*），不能靠值判空。
    DictEntry* e = d.Find(key);
    mycache::sdsfree(key);
    return e ? reinterpret_cast<long>(e->val) : -1;
  }
  bool Del(const std::string& k) {
    sds key = NewKey(k);
    bool ok = d.Delete(key);
    mycache::sdsfree(key);
    return ok;
  }
};

}  // namespace

TEST(DictTest, BasicAddFindDelete) {
  TestDict d;
  EXPECT_EQ(d.Get("k"), -1);
  EXPECT_EQ(d.Set("k", 100), 1);
  EXPECT_EQ(d.Get("k"), 100);
  EXPECT_EQ(d.d.Size(), 1u);
  EXPECT_TRUE(d.Del("k"));
  EXPECT_EQ(d.d.Size(), 0u);
  EXPECT_EQ(d.Get("k"), -1);
}

TEST(DictTest, AddExistingDoesNotOverwrite) {
  TestDict d;
  EXPECT_EQ(d.Set("a", 1), 1);
  EXPECT_EQ(d.Set("a", 2), 0);  // 已存在，Add 返回 0
  EXPECT_EQ(d.Get("a"), 1);     // 值未变
  EXPECT_EQ(d.Replace("a", 3), 0);  // 替换
  EXPECT_EQ(d.Get("a"), 3);
  EXPECT_EQ(d.d.Size(), 1u);
}

TEST(DictTest, ManyKeys) {
  TestDict d;
  const int N = 1000;
  for (int i = 0; i < N; ++i) d.Set("key" + std::to_string(i), i);
  EXPECT_EQ(d.d.Size(), static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) EXPECT_EQ(d.Get("key" + std::to_string(i)), i);
  for (int i = 0; i < N; i += 2) EXPECT_TRUE(d.Del("key" + std::to_string(i)));
  for (int i = 0; i < N; ++i) {
    if (i % 2 == 0) EXPECT_EQ(d.Get("key" + std::to_string(i)), -1);
    else EXPECT_EQ(d.Get("key" + std::to_string(i)), i);
  }
}

TEST(DictTest, GrowAndRehashPreservesKeys) {
  TestDict d;
  const int N = 2000;
  for (int i = 0; i < N; ++i) d.Set("k" + std::to_string(i), i);
  // 驱散可能仍在进行中的 rehash。
  while (d.d.Rehashing()) d.d.Rehash(1000);
  EXPECT_FALSE(d.d.Rehashing());
  EXPECT_EQ(d.d.Size(), static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) EXPECT_EQ(d.Get("k" + std::to_string(i)), i);
}

TEST(DictTest, ShrinkOnManyDeletes) {
  TestDict d;
  const int N = 100;
  for (int i = 0; i < N; ++i) d.Set("k" + std::to_string(i), i);
  while (d.d.Rehashing()) d.d.Rehash(1000);
  const size_t before = d.d.Table(0).size;
  ASSERT_GE(before, 128u);

  for (int i = 0; i < N - 10; ++i) d.Del("k" + std::to_string(i));
  while (d.d.Rehashing()) d.d.Rehash(1000);
  EXPECT_LT(d.d.Table(0).size, before);
  for (int i = N - 10; i < N; ++i) EXPECT_EQ(d.Get("k" + std::to_string(i)), i);
}
