#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "mycache/storage/db/shard.h"

using mycache::Shard;

TEST(TtlTest, ExpireZeroDeletesOnNextAccess) {
  Shard shard;
  shard.Set("k", "v");
  EXPECT_TRUE(shard.ExpireMs("k", 0));  // 立即到期

  std::string v;
  EXPECT_FALSE(shard.Get("k", &v));  // 惰性删除后不可见
  EXPECT_EQ(shard.TtlMs("k"), -2);
  EXPECT_FALSE(shard.Exists("k"));
}

TEST(TtlTest, ExpireMissingKey) {
  Shard shard;
  EXPECT_FALSE(shard.ExpireMs("nosuch", 1000));
  EXPECT_EQ(shard.TtlMs("nosuch"), -2);
}

TEST(TtlTest, SetClearsTtl) {
  Shard shard;
  shard.Set("k", "v");
  EXPECT_TRUE(shard.ExpireMs("k", 60 * 1000));
  EXPECT_GT(shard.TtlMs("k"), 0);

  shard.Set("k", "v2");  // SET 覆盖清除 TTL
  EXPECT_EQ(shard.TtlMs("k"), -1);
}

TEST(TtlTest, KeyExpiresAfterDelay) {
  Shard shard;
  shard.Set("k", "v");
  EXPECT_TRUE(shard.ExpireMs("k", 30));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  std::string v;
  EXPECT_FALSE(shard.Get("k", &v));  // 到期后不可见
  EXPECT_EQ(shard.TtlMs("k"), -2);
}

TEST(TtlTest, ActiveExpireRemovesExpiredKeys) {
  Shard shard;
  // 写入一批 key，并给每个 key 设置一个已过期的 TTL（相对 0 等价于立即到期）。
  for (int i = 0; i < 100; ++i) {
    shard.Set("key" + std::to_string(i), "v");
    shard.ExpireMs("key" + std::to_string(i), 0);
  }
  // 主动过期是随机采样，需多轮才能逐步清空。
  for (int i = 0; i < 500 && shard.Size() > 0; ++i) {
    shard.ActiveExpireCycle();
  }
  EXPECT_EQ(shard.Size(), 0u);
}
