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

sds MakeKey(const std::string& k) { return mycache::sdsnewlen(k.data(), k.size()); }

void AddKey(Dict& d, const std::string& k, long v) {
  sds key = MakeKey(k);
  ASSERT_EQ(d.Add(key, reinterpret_cast<void*>(v)), 1);
  mycache::sdsfree(key);
}

// 用条目指针判断存在性：值可能为 0（void* 的 null），不能靠值判空。
bool Lookup(Dict& d, const std::string& k, long* out) {
  sds key = MakeKey(k);
  DictEntry* e = d.Find(key);
  mycache::sdsfree(key);
  if (!e) return false;
  *out = reinterpret_cast<long>(e->val);
  return true;
}

}  // namespace

// D4 的核心验收：渐进式 rehash 期间，读写始终正确。
TEST(RehashTest, ReadWriteCorrectDuringPartialRehash) {
  Dict d(TestType());
  const int N = 1000;
  for (int i = 0; i < N; ++i) AddKey(d, "key" + std::to_string(i), i);

  // 手动扩容，强制进入 rehash 状态。
  ASSERT_EQ(d.Expand(2048), 0);
  ASSERT_TRUE(d.Rehashing());

  // 只迁移一小部分桶，保持在 rehash 中途。
  d.Rehash(10);
  ASSERT_TRUE(d.Rehashing());

  // rehash 中途读：键可能落在 ht[0] 或 ht[1]，都必须能查到。
  for (int i = 0; i < N; ++i) {
    long v = -1;
    ASSERT_TRUE(Lookup(d, "key" + std::to_string(i), &v)) << "missing key " << i;
    EXPECT_EQ(v, i);
  }

  // rehash 中途写：覆盖既有键 + 插入新键。
  {
    sds k = MakeKey("key500");
    EXPECT_EQ(d.Replace(k, reinterpret_cast<void*>(999L)), 0);
    mycache::sdsfree(k);
  }
  AddKey(d, "newkey", -7);

  // rehash 中途删：删除某个既有键。
  {
    sds k = MakeKey("key123");
    EXPECT_TRUE(d.Delete(k));
    mycache::sdsfree(k);
  }

  // 完成 rehash。
  while (d.Rehashing()) d.Rehash(100);
  ASSERT_FALSE(d.Rehashing());
  EXPECT_EQ(d.Size(), static_cast<size_t>(N + 1 - 1));  // N - 1 删除 + 1 新增

  // 最终一致性核对。
  for (int i = 0; i < N; ++i) {
    if (i == 123) continue;
    long expect = (i == 500) ? 999 : i;
    long v = -1;
    ASSERT_TRUE(Lookup(d, "key" + std::to_string(i), &v)) << "missing key " << i;
    EXPECT_EQ(v, expect);
  }
  long nv = 0;
  EXPECT_TRUE(Lookup(d, "newkey", &nv));
  EXPECT_EQ(nv, -7);
}

TEST(RehashTest, AddDuringRehashGoesToHt1) {
  Dict d(TestType());
  for (int i = 0; i < 100; ++i) AddKey(d, "k" + std::to_string(i), i);
  // 先驱散可能已在进行中的 rehash，得到一个稳定状态。
  while (d.Rehashing()) d.Rehash(1000);
  ASSERT_FALSE(d.Rehashing());

  ASSERT_EQ(d.Expand(512), 0);
  ASSERT_TRUE(d.Rehashing());

  // rehash 中新增键必须进入 ht[1]（写路径先 RehashStep 再写入目标表 ht[1]）。
  AddKey(d, "fresh", 7);

  // 直接在 ht[1] 中定位 fresh：rehash 只从 ht[0] 迁出，ht[1] 里的键不会回移。
  {
    sds fk = MakeKey("fresh");
    uint64_t h = SdsHash(fk);
    bool in_ht1 = false;
    for (DictEntry* e = d.Table(1).table[h & d.Table(1).sizemask]; e; e = e->next) {
      if (SdsEq(e->key, fk)) in_ht1 = true;
    }
    mycache::sdsfree(fk);
    EXPECT_TRUE(in_ht1);
  }

  while (d.Rehashing()) d.Rehash(1000);
  ASSERT_FALSE(d.Rehashing());
  EXPECT_EQ(d.Size(), 101u);
}

TEST(RehashTest, RehashCompletesAndSwapsTables) {
  Dict d(TestType());
  const int N = 100;
  for (int i = 0; i < N; ++i) AddKey(d, "k" + std::to_string(i), i);

  while (d.Rehashing()) d.Rehash(1000);
  ASSERT_FALSE(d.Rehashing());
  // 100 个键收敛到 2^ceil(log2(100)) = 128 个桶。
  EXPECT_EQ(d.Table(0).size, 128u);
  EXPECT_EQ(d.Table(1).size, 0u);
  EXPECT_EQ(d.Table(0).used, static_cast<size_t>(N));

  for (int i = 0; i < N; ++i) {
    long v = -1;
    ASSERT_TRUE(Lookup(d, "k" + std::to_string(i), &v));
    EXPECT_EQ(v, i);
  }
}

// const Find 必须保持纯只读：反复查询也不推进 rehash（这是共享锁读路径
// 成立的契约）。
TEST(RehashTest, ReadOnlyFindDoesNotAdvanceRehash) {
  Dict d(TestType());
  const int N = 1000;
  for (int i = 0; i < N; ++i) AddKey(d, "key" + std::to_string(i), i);
  while (d.Rehashing()) d.Rehash(1000);  // 先收敛到稳定态
  ASSERT_FALSE(d.Rehashing());

  ASSERT_EQ(d.Expand(2048), 0);
  ASSERT_TRUE(d.Rehashing());
  const long idx_before = d.RehashIndex();

  for (int i = 0; i < N; ++i) {
    sds k = MakeKey("key" + std::to_string(i));
    DictEntry* e = d.Find(k);
    mycache::sdsfree(k);
    ASSERT_NE(e, nullptr) << "missing key " << i;
  }
  EXPECT_EQ(d.RehashIndex(), idx_before);  // Find 不推进 rehash
  EXPECT_TRUE(d.Rehashing());
}

// 分片后读路径用 FindAndRehash：只读（无 Add/Delete）也必须能把 rehash 搬完，
// 否则读多写少的负载下 rehash 永远结束不了。
TEST(RehashTest, FindAndRehashDrainsRehash) {
  Dict d(TestType());
  const int N = 1000;
  for (int i = 0; i < N; ++i) AddKey(d, "key" + std::to_string(i), i);
  while (d.Rehashing()) d.Rehash(1000);
  ASSERT_FALSE(d.Rehashing());

  ASSERT_EQ(d.Expand(2048), 0);
  ASSERT_TRUE(d.Rehashing());

  // 只走读路径推进，不调用 Add/Delete/Replace。
  int guard = 0;
  const int kMaxSteps = 200000;  // 上限防死循环；每步推进 1 个桶
  while (d.Rehashing() && guard++ < kMaxSteps) {
    sds k = MakeKey("key" + std::to_string(guard % N));
    DictEntry* e = d.FindAndRehash(k);
    mycache::sdsfree(k);
    ASSERT_NE(e, nullptr) << "missing key during rehash";
  }
  EXPECT_FALSE(d.Rehashing());

  // rehash 结束后数据完整、值正确。
  for (int i = 0; i < N; ++i) {
    long v = -1;
    ASSERT_TRUE(Lookup(d, "key" + std::to_string(i), &v));
    EXPECT_EQ(v, i);
  }
}
