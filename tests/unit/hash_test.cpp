#include <map>
#include <string>

#include <gtest/gtest.h>

#include "mycache/storage/db/shard.h"

using mycache::Shard;

TEST(HashTest, HSetHGetOverwrite) {
  Shard shard;
  EXPECT_EQ(shard.HSet("h", "f1", "v1"), 1);  // 新增字段
  EXPECT_EQ(shard.HSet("h", "f1", "v2"), 0);  // 覆盖已有字段

  std::string v;
  EXPECT_TRUE(shard.HGet("h", "f1", &v));
  EXPECT_EQ(v, "v2");
  EXPECT_FALSE(shard.HGet("h", "missing", &v));

  EXPECT_EQ(shard.Type("h"), "hash");
}

TEST(HashTest, HDelAndEmptyKeyRemoval) {
  Shard shard;
  shard.HSet("h", "a", "1");
  shard.HSet("h", "b", "2");
  EXPECT_EQ(shard.HDel("h", {"a"}), 1);
  EXPECT_EQ(shard.HGetAll("h").size(), 1u);
  EXPECT_EQ(shard.HDel("h", {"b"}), 1);
  // 字段清空后，hash key 应被删除（与 Redis 一致）。
  EXPECT_EQ(shard.Type("h"), "none");
  EXPECT_EQ(shard.HDel("h", {"a"}), 0);
}

TEST(HashTest, HGetAll) {
  Shard shard;
  shard.HSet("h", "a", "1");
  shard.HSet("h", "b", "2");
  shard.HSet("h", "c", "3");
  auto all = shard.HGetAll("h");
  EXPECT_EQ(all.size(), 3u);

  std::map<std::string, std::string> m(all.begin(), all.end());
  EXPECT_EQ(m["a"], "1");
  EXPECT_EQ(m["b"], "2");
  EXPECT_EQ(m["c"], "3");
}

TEST(HashTest, MissingKeyBehavesAsEmpty) {
  Shard shard;
  std::string v;
  EXPECT_FALSE(shard.HGet("nosuch", "f", &v));
  EXPECT_TRUE(shard.HGetAll("nosuch").empty());
  EXPECT_EQ(shard.HDel("nosuch", {"f"}), 0);
}

TEST(HashTest, WrongType) {
  Shard shard;
  shard.Set("k", "v");
  EXPECT_EQ(shard.Type("k"), "string");
  EXPECT_EQ(shard.HSet("k", "f", "v"), -1);  // WRONGTYPE
  std::string v;
  EXPECT_FALSE(shard.HGet("k", "f", &v));
}

TEST(HashTest, ManyFieldsRoundTrip) {
  Shard shard;
  const int N = 500;
  for (int i = 0; i < N; ++i) {
    EXPECT_EQ(shard.HSet("big", "field" + std::to_string(i), std::to_string(i)), 1);
  }
  EXPECT_EQ(shard.HGetAll("big").size(), static_cast<size_t>(N));
  for (int i = 0; i < N; i += 3) {
    std::string v;
    EXPECT_TRUE(shard.HGet("big", "field" + std::to_string(i), &v));
    EXPECT_EQ(v, std::to_string(i));
  }
}
