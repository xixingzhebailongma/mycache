#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mycache/storage/db/shard.h"

using mycache::Shard;

TEST(ListTest, LPushAndLRange) {
  Shard shard;
  shard.LPush("l", {"a", "b", "c"});  // 依次头插 → c, b, a
  auto vals = shard.LRange("l", 0, -1);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], "c");
  EXPECT_EQ(vals[1], "b");
  EXPECT_EQ(vals[2], "a");
}

TEST(ListTest, RPushAppends) {
  Shard shard;
  shard.RPush("l", {"a"});
  shard.RPush("l", {"b"});
  auto vals = shard.LRange("l", 0, -1);
  ASSERT_EQ(vals.size(), 2u);
  EXPECT_EQ(vals[0], "a");
  EXPECT_EQ(vals[1], "b");
}

TEST(ListTest, PopFromFront) {
  Shard shard;
  shard.RPush("l", {"a", "b", "c"});
  std::string v;
  EXPECT_TRUE(shard.LPop("l", &v));
  EXPECT_EQ(v, "a");
  EXPECT_EQ(shard.LLen("l"), 2u);
}

TEST(ListTest, PopToEmptyDeletesKey) {
  Shard shard;
  shard.RPush("l", {"only"});
  std::string v;
  EXPECT_TRUE(shard.LPop("l", &v));
  EXPECT_EQ(v, "only");
  EXPECT_EQ(shard.LLen("l"), 0u);
  EXPECT_EQ(shard.Type("l"), "none");  // 空列表删除 key
}

TEST(ListTest, NegativeIndexLRange) {
  Shard shard;
  shard.RPush("l", {"a", "b", "c", "d"});
  auto vals = shard.LRange("l", -2, -1);
  ASSERT_EQ(vals.size(), 2u);
  EXPECT_EQ(vals[0], "c");
  EXPECT_EQ(vals[1], "d");
}

TEST(ListTest, WrongType) {
  Shard shard;
  shard.Set("s", "v");
  EXPECT_EQ(shard.LPush("s", {"x"}), -1);  // 非 list → WRONGTYPE
  EXPECT_EQ(shard.LLen("s"), 0u);
}
