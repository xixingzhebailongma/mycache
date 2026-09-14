#include <string>

#include <gtest/gtest.h>

#include "mycache/storage/memory/memory_stats.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/object/string_object.h"

using mycache::ObjEncoding;
using mycache::Object;

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
