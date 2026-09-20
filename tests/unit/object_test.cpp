#include <string>

#include <gtest/gtest.h>

#include "mycache/eviction/eviction_policy.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/memory/memory_stats.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/object/string_object.h"

using mycache::ObjEncoding;
using mycache::Object;
using mycache::Shard;

TEST(ObjectTest, StringEncodingRoundtrip) {
  // 短、长字符串都使用 string 编码（内容存放在独立分配的 sds 中）。
  Object* a = mycache::CreateStringObject("short", 5);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->type, mycache::ObjType::kString);
  EXPECT_EQ(a->encoding, ObjEncoding::kString);
  EXPECT_EQ(a->refcount, 1);
  EXPECT_EQ(a->ToString(), "short");
  a->DecrRefCount();

  std::string big(100, 'x');
  Object* b = mycache::CreateStringObject(big.data(), big.size());
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->encoding, ObjEncoding::kString);
  EXPECT_EQ(b->ToString(), big);
  b->DecrRefCount();
}

TEST(ObjectTest, IntEncoding) {
  Object* o = mycache::CreateIntObject(12345);
  EXPECT_EQ(o->encoding, ObjEncoding::kInt);
  EXPECT_EQ(o->ToString(), "12345");
  long long v = 0;
  EXPECT_TRUE(o->TryGetLongLong(&v));
  EXPECT_EQ(v, 12345);
  o->DecrRefCount();
}

TEST(ObjectTest, TryObjectEncodingConvertsToInt) {
  Object* o = mycache::CreateStringObject("12345", 5);
  o = mycache::TryObjectEncoding(o);
  EXPECT_EQ(o->encoding, ObjEncoding::kInt);
  EXPECT_EQ(o->ToString(), "12345");
  o->DecrRefCount();
}

TEST(ObjectTest, TryObjectEncodingRejectsNonInteger) {
  Object* o = mycache::CreateStringObject("12a", 3);
  o = mycache::TryObjectEncoding(o);
  EXPECT_NE(o->encoding, ObjEncoding::kInt);
  o->DecrRefCount();

  Object* o2 = mycache::CreateStringObject("0123", 4);  // 前导零
  o2 = mycache::TryObjectEncoding(o2);
  EXPECT_NE(o2->encoding, ObjEncoding::kInt);
  o2->DecrRefCount();
}

TEST(ObjectTest, RefCount) {
  Object* o = mycache::CreateStringObject("hello", 5);
  EXPECT_EQ(o->refcount, 1);
  o->IncrRefCount();
  EXPECT_EQ(o->refcount, 2);
  o->DecrRefCount();
  EXPECT_EQ(o->refcount, 1);
  o->DecrRefCount();  // 释放
}

TEST(ObjectTest, StringToLongLongStrict) {
  long long v = 0;
  EXPECT_TRUE(mycache::StringToLongLong("0", 1, &v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(mycache::StringToLongLong("-123", 4, &v));
  EXPECT_EQ(v, -123);
  EXPECT_TRUE(mycache::StringToLongLong("9223372036854775807", 19, &v));
  EXPECT_EQ(v, 9223372036854775807LL);
  EXPECT_TRUE(mycache::StringToLongLong("-9223372036854775808", 20, &v));
  EXPECT_EQ(v, -9223372036854775807LL - 1);

  EXPECT_FALSE(mycache::StringToLongLong("00", 2, &v));
  EXPECT_FALSE(mycache::StringToLongLong("-0", 2, &v));
  EXPECT_FALSE(mycache::StringToLongLong("+5", 2, &v));
  EXPECT_FALSE(mycache::StringToLongLong(" 5", 2, &v));
  EXPECT_FALSE(mycache::StringToLongLong("5 ", 2, &v));
  EXPECT_FALSE(mycache::StringToLongLong("", 0, &v));
  EXPECT_FALSE(mycache::StringToLongLong("9223372036854775808", 19, &v));   // 溢出
  EXPECT_FALSE(mycache::StringToLongLong("-9223372036854775809", 20, &v));  // 溢出
}

TEST(MemoryStatsTest, StringCountTracksCreateFree) {
  size_t before = mycache::MemoryStats::Instance().TotalStrings();
  Object* o = mycache::CreateStringObject("x", 1);
  EXPECT_EQ(mycache::MemoryStats::Instance().TotalStrings(), before + 1);
  o->DecrRefCount();
  EXPECT_EQ(mycache::MemoryStats::Instance().TotalStrings(), before);
  EXPECT_FALSE(mycache::MemoryStats::Instance().Summary().empty());
}

// ---- 对象共享（Week 2 D5）：让引用计数的「同对象 SET」保护真正被用到 ----

TEST(SharedIntegerTest, TryObjectEncodingSharesSmallInteger) {
  mycache::CreateSharedObjects();
  Object* o = mycache::CreateStringObject("100", 3);
  Object* encoded = mycache::TryObjectEncoding(o, true);
  ASSERT_NE(encoded, nullptr);
  EXPECT_EQ(encoded, mycache::g_shared.integers[100]);  // 复用全局共享对象
  EXPECT_EQ(encoded->encoding, ObjEncoding::kInt);
  EXPECT_EQ(encoded->ToString(), "100");
  // 共享对象 refcount 钉在 INT_MAX，DecrRefCount 不递减、不释放。
  EXPECT_EQ(encoded->refcount, mycache::kSharedRefCount);
  encoded->DecrRefCount();
  EXPECT_EQ(encoded->refcount, mycache::kSharedRefCount);
}

TEST(SharedIntegerTest, TwoEncodingsReturnSamePointer) {
  mycache::CreateSharedObjects();
  Object* a = mycache::TryObjectEncoding(mycache::CreateStringObject("100", 3), true);
  Object* b = mycache::TryObjectEncoding(mycache::CreateStringObject("100", 3), true);
  EXPECT_EQ(a, b);  // 两次编码拿到同一个指针 —— 正是「SET 提取到和原来一样东西」
  a->DecrRefCount();
  b->DecrRefCount();
}

TEST(SharedIntegerTest, SharingDisabledFallsBackToInlineInt) {
  mycache::CreateSharedObjects();
  Object* o = mycache::CreateStringObject("100", 3);
  Object* encoded = mycache::TryObjectEncoding(o, false);  // 禁止共享
  EXPECT_NE(encoded, mycache::g_shared.integers[100]);
  EXPECT_EQ(encoded, o);  // 原地改写为 int
  EXPECT_EQ(encoded->encoding, ObjEncoding::kInt);
  encoded->DecrRefCount();
}

TEST(SharedIntegerTest, OutOfRangeNotShared) {
  mycache::CreateSharedObjects();
  Object* o = mycache::CreateStringObject("10000", 5);  // 10000 == kSharedIntegers，越界
  Object* encoded = mycache::TryObjectEncoding(o, true);
  EXPECT_EQ(encoded, o);  // 仍原地改写为 int（非共享）
  EXPECT_EQ(encoded->encoding, ObjEncoding::kInt);
  encoded->DecrRefCount();
}

TEST(SharedIntegerTest, AllowSharedIntegersPolicy) {
  mycache::EvictionConfig noeviction;  // 默认 maxmemory 0 / noeviction
  EXPECT_TRUE(mycache::AllowSharedIntegers(noeviction));

  mycache::EvictionConfig lru;
  lru.policy = mycache::EvictionPolicy::kAllkeysLru;
  EXPECT_FALSE(mycache::AllowSharedIntegers(lru));

  mycache::EvictionConfig random;
  random.policy = mycache::EvictionPolicy::kAllkeysRandom;
  EXPECT_TRUE(mycache::AllowSharedIntegers(random));
}

TEST(SharedIntegerTest, ShardSetSameIntegerTwiceIsSafe) {
  mycache::CreateSharedObjects();
  Shard shard;
  shard.Set("k", "100");
  shard.Set("k", "100");  // 第二次 SET：旧值 == 新值（同一共享对象），覆盖必须安全
  std::string v;
  ASSERT_TRUE(shard.Get("k", &v));
  EXPECT_EQ(v, "100");
  shard.Del({"k"});
}
