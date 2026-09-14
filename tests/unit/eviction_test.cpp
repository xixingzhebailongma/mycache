#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "mycache/eviction/eviction_policy.h"
#include "mycache/eviction/lru.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/memory/allocator.h"

using mycache::EvictionConfig;
using mycache::EvictionPolicy;
using mycache::RefreshLruClock;
using mycache::Shard;

namespace {
EvictionConfig LruConfig() {
  EvictionConfig cfg;
  cfg.policy = EvictionPolicy::kAllkeysLru;
  cfg.maxmemory_samples = 10;
  return cfg;
}
}  // namespace

TEST(EvictionPolicyTest, Parse) {
  EvictionPolicy p;
  EXPECT_TRUE(mycache::ParseEvictionPolicy("noeviction", &p));
  EXPECT_EQ(p, EvictionPolicy::kNoEviction);
  EXPECT_TRUE(mycache::ParseEvictionPolicy("allkeys-lru", &p));
  EXPECT_EQ(p, EvictionPolicy::kAllkeysLru);
  EXPECT_TRUE(mycache::ParseEvictionPolicy("volatile-random", &p));
  EXPECT_EQ(p, EvictionPolicy::kVolatileRandom);
  EXPECT_FALSE(mycache::ParseEvictionPolicy("bogus", &p));
}

TEST(EvictionTest, NoEvictionRefusesWhenOver) {
  Shard shard;
  size_t base = mycache::used_memory();
  EvictionConfig cfg;
  cfg.policy = EvictionPolicy::kNoEviction;
  cfg.maxmemory = static_cast<int64_t>(base + 1);  // 写入一个 key 即超限
  shard.ConfigureEviction(cfg);

  EXPECT_TRUE(shard.EvictIfNeeded());  // 尚未超限
  shard.Set("k", "v");                 // 现在超限
  EXPECT_FALSE(shard.EvictIfNeeded()); // noeviction 拒绝写入
  std::string v;
  EXPECT_TRUE(shard.Get("k", &v));     // 已写入的 key 仍在
}

TEST(EvictionTest, AllkeysRandomFreesMemoryWhenOver) {
  Shard shard;
  // 写入一批 key，记录写入后的内存与 key 数。
  for (int i = 0; i < 100; ++i) shard.Set("k" + std::to_string(i), "v");
  size_t size_before = shard.Size();
  size_t mem_before = mycache::used_memory();

  // 设置一个低于当前内存的上限，触发淘汰。
  EvictionConfig cfg;
  cfg.policy = EvictionPolicy::kAllkeysRandom;
  cfg.maxmemory_samples = 10;
  cfg.maxmemory = static_cast<int64_t>(mem_before - 1);
  shard.ConfigureEviction(cfg);

  shard.EvictIfNeeded();
  EXPECT_LT(shard.Size(), size_before);          // 淘汰了 key
  EXPECT_LT(mycache::used_memory(), mem_before); // 内存下降了
}

TEST(EvictionTest, AllkeysLruEvictsOldest) {
  Shard shard;
  shard.ConfigureEviction(LruConfig());

  // 用不同时间戳写入，制造 lru 差异：a 最旧，b 次旧，c 最新。
  RefreshLruClock();
  shard.Set("a", "1");
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  RefreshLruClock();
  shard.Set("b", "2");
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  RefreshLruClock();
  shard.Set("c", "3");

  // 访问 a，使其变成最新；此时 b 最旧。
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  RefreshLruClock();
  std::string tmp;
  shard.Get("a", &tmp);

  EXPECT_TRUE(shard.EvictOne());  // 应淘汰最旧的 b

  std::string out;
  EXPECT_TRUE(shard.Get("a", &out));
  EXPECT_TRUE(shard.Get("c", &out));
  EXPECT_FALSE(shard.Get("b", &out));
}

TEST(EvictionTest, VolatileOnlyEvictsTtlKeys) {
  Shard shard;
  EvictionConfig cfg;
  cfg.policy = EvictionPolicy::kVolatileRandom;
  cfg.maxmemory_samples = 20;
  shard.ConfigureEviction(cfg);

  shard.Set("nottl1", "v");
  shard.Set("nottl2", "v");
  shard.Set("ttl1", "v");
  shard.ExpireMs("ttl1", 100000);
  shard.Set("ttl2", "v");
  shard.ExpireMs("ttl2", 100000);

  // 反复淘汰直到没有可淘汰对象。
  for (int i = 0; i < 100; ++i) {
    if (!shard.EvictOne()) break;
  }

  std::string out;
  EXPECT_TRUE(shard.Get("nottl1", &out));   // 无 TTL 的 key 必须存活
  EXPECT_TRUE(shard.Get("nottl2", &out));
  EXPECT_FALSE(shard.Get("ttl1", &out));    // 带 TTL 的 key 被淘汰
  EXPECT_FALSE(shard.Get("ttl2", &out));
}

TEST(EvictionTest, KeyCountsTrackOps) {
  Shard shard;
  EXPECT_EQ(shard.Size(), 0u);
  EXPECT_EQ(shard.ExpireCount(), 0u);

  shard.Set("a", "1");
  shard.Set("b", "2");
  EXPECT_EQ(shard.Size(), 2u);

  shard.Del({"a"});
  EXPECT_EQ(shard.Size(), 1u);

  shard.ExpireMs("b", 100000);
  EXPECT_EQ(shard.ExpireCount(), 1u);

  shard.Set("b", "3");  // SET 清除 TTL
  EXPECT_EQ(shard.ExpireCount(), 0u);
  EXPECT_EQ(shard.Size(), 1u);
}
