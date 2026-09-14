#include <string>

#include <gtest/gtest.h>

#include "mycache/storage/string/sds.h"

using mycache::sds;

namespace {

std::string ToString(const sds s) { return std::string(s, mycache::sdslen(s)); }

}  // namespace

TEST(SdsTest, NewLenAndEmpty) {
  sds s = mycache::sdsnewlen("hello", 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(mycache::sdslen(s), 5u);
  EXPECT_STREQ(s, "hello");
  EXPECT_EQ(ToString(s), "hello");
  mycache::sdsfree(s);

  sds e = mycache::sdsempty();
  EXPECT_EQ(mycache::sdslen(e), 0u);
  EXPECT_STREQ(e, "");
  mycache::sdsfree(e);
}

TEST(SdsTest, BinarySafe) {
  const char data[] = {'a', '\0', 'b'};
  sds s = mycache::sdsnewlen(data, 3);
  EXPECT_EQ(mycache::sdslen(s), 3u);
  EXPECT_EQ(std::string(s, 3), std::string(data, 3));
  mycache::sdsfree(s);
}

TEST(SdsTest, Dup) {
  sds a = mycache::sdsnew("dup-me");
  sds b = mycache::sdsdup(a);
  EXPECT_STREQ(b, "dup-me");
  EXPECT_EQ(mycache::sdslen(b), 6u);
  mycache::sdsfree(a);
  mycache::sdsfree(b);
}

TEST(SdsTest, CatAndGrow) {
  sds s = mycache::sdsempty();
  for (int i = 0; i < 1000; ++i) s = mycache::sdscat(s, "x");
  EXPECT_EQ(mycache::sdslen(s), 1000u);
  EXPECT_EQ(ToString(s), std::string(1000, 'x'));
  mycache::sdsfree(s);
}

TEST(SdsTest, Copy) {
  sds s = mycache::sdsnew("a very long initial string");
  s = mycache::sdscpy(s, "short");
  EXPECT_STREQ(s, "short");
  EXPECT_EQ(mycache::sdslen(s), 5u);
  mycache::sdsfree(s);
}

TEST(SdsTest, HeaderTypeUpgrade) {
  // 制造足够长的字符串，迫使头从 sdshdr8 升级到 sdshdr16。
  sds s = mycache::sdsempty();
  std::string big(300, 'y');
  s = mycache::sdscatlen(s, big.data(), big.size());
  EXPECT_EQ(mycache::sdslen(s), 300u);
  EXPECT_EQ(ToString(s), big);

  // 追加并覆盖，验证升级后读写正确。
  s = mycache::sdscat(s, "!");
  EXPECT_EQ(mycache::sdslen(s), 301u);
  EXPECT_EQ(ToString(s), big + "!");
  mycache::sdsfree(s);
}

TEST(SdsTest, FromLongLong) {
  sds s = mycache::sdsfromlonglong(-9223372036854775807LL - 1);
  EXPECT_STREQ(s, "-9223372036854775808");
  mycache::sdsfree(s);

  sds p = mycache::sdsfromlonglong(12345);
  EXPECT_STREQ(p, "12345");
  mycache::sdsfree(p);
}

TEST(SdsTest, Compare) {
  sds a = mycache::sdsnew("abc");
  sds b = mycache::sdsnew("abd");
  sds c = mycache::sdsnew("abc");
  EXPECT_LT(mycache::sdscmp(a, b), 0);
  EXPECT_GT(mycache::sdscmp(b, a), 0);
  EXPECT_EQ(mycache::sdscmp(a, c), 0);
  mycache::sdsfree(a);
  mycache::sdsfree(b);
  mycache::sdsfree(c);
}
