#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mycache/command/multi_key.h"
#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"

using mycache::CommandRouter;
using mycache::EvictionConfig;
using mycache::FanOut;
using mycache::GroupKeysByShard;
using mycache::KeyGroup;
using mycache::Shard;

namespace {

// 等待条件成立（最多 ~1s）。
void WaitFor(const std::atomic<bool>& done) {
  for (int i = 0; i < 1000 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

// 每个 key 恰好落到一个 group；group 的 shard_index 与 ShardIndex 一致；indices
// 无重复且覆盖所有 key 下标。
TEST(MultiKeyTest, GroupKeysByShardCoverage) {
  CommandRouter router(8, EvictionConfig{});
  std::vector<std::string> keys = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};
  auto groups = GroupKeysByShard(router, keys);

  std::vector<bool> seen(keys.size(), false);
  for (const auto& g : groups) {
    for (size_t i : g.indices) {
      ASSERT_LT(i, keys.size());
      EXPECT_FALSE(seen[i]) << "key index " << i << " covered twice";
      seen[i] = true;
      EXPECT_EQ(g.shard_index, router.ShardIndex(keys[i]));
    }
  }
  for (size_t i = 0; i < seen.size(); ++i) {
    EXPECT_TRUE(seen[i]) << "key index " << i << " never covered";
  }
}

// FanOut 求和语义（DEL/EXISTS 的跨 shard 计数），finish 恰好调用一次。
TEST(MultiKeyTest, FanOutAggregatesSumAndFinishesOnce) {
  CommandRouter router(8, EvictionConfig{});
  router.Start();

  std::vector<std::string> keys;
  for (int i = 0; i < 64; ++i) keys.push_back("k" + std::to_string(i));
  auto groups = GroupKeysByShard(router, keys);

  // 先把 64 个 key 写入各自 shard（RouteTo 异步，用原子计数等待）。
  std::atomic<int> pending{0};
  for (const auto& g : groups) {
    pending.fetch_add(1);
    router.RouteTo(g.shard_index, [&keys, g, &pending](Shard* shard) {
      for (size_t i : g.indices) shard->Set(keys[i], "v");
      pending.fetch_sub(1);
    });
  }
  while (pending.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  // FanOut 计算存在数。
  auto total = std::make_shared<std::atomic<int64_t>>(0);
  std::atomic<bool> finished{false};
  std::atomic<int> finish_count{0};
  FanOut(&router, GroupKeysByShard(router, keys),
         [&keys, total](Shard* shard, const KeyGroup& g) {
           int n = 0;
           for (size_t i : g.indices) {
             if (shard->Exists(keys[i])) ++n;
           }
           total->fetch_add(n);
         },
         [total, &finished, &finish_count]() {
           finish_count.fetch_add(1);
           finished.store(true);
         });

  WaitFor(finished);
  EXPECT_TRUE(finished.load());
  EXPECT_EQ(finish_count.load(), 1);
  EXPECT_EQ(total->load(), static_cast<int64_t>(keys.size()));

  router.Stop();
}

// FanOut 按位回填语义（MGET：结果按 key 在原参数中的位置排列）。
TEST(MultiKeyTest, FanOutFillsByPosition) {
  CommandRouter router(8, EvictionConfig{});
  router.Start();

  std::vector<std::string> keys = {"a", "b", "c", "d", "e", "f", "g", "h"};
  // 只写偶数下标的 key：b、d、f、h 缺失。
  auto groups = GroupKeysByShard(router, keys);
  std::atomic<int> pending{0};
  for (const auto& g : groups) {
    pending.fetch_add(1);
    router.RouteTo(g.shard_index, [&keys, g, &pending](Shard* shard) {
      for (size_t i : g.indices) {
        if (i % 2 == 0) shard->Set(keys[i], "val" + std::to_string(i));
      }
      pending.fetch_sub(1);
    });
  }
  while (pending.load() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  auto results = std::make_shared<std::vector<std::string>>(keys.size(), "<nil>");
  std::atomic<bool> finished{false};
  FanOut(&router, GroupKeysByShard(router, keys),
         [&keys, results](Shard* shard, const KeyGroup& g) {
           for (size_t i : g.indices) {
             std::string v;
             if (shard->Get(keys[i], &v)) (*results)[i] = v;
           }
         },
         [&finished]() { finished.store(true); });

  WaitFor(finished);
  EXPECT_TRUE(finished.load());
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i % 2 == 0) {
      EXPECT_EQ((*results)[i], "val" + std::to_string(i));
    } else {
      EXPECT_EQ((*results)[i], "<nil>");  // 缺失 key 保持占位
    }
  }

  router.Stop();
}
