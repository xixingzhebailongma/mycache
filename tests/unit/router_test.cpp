#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "mycache/server/command_router.h"
#include "mycache/server/shard_worker.h"
#include "mycache/storage/db/shard.h"

using mycache::CommandRouter;
using mycache::Shard;
using mycache::ShardWorker;

TEST(RouterTest, ShardIndexDeterministicAndInRange) {
  CommandRouter router(8, mycache::EvictionConfig{});
  for (const std::string key : {"a", "b", "hello", "world", "key:12345", ""}) {
    EXPECT_EQ(router.ShardIndex(key), router.ShardIndex(key));
    EXPECT_LT(router.ShardIndex(key), router.shard_count());
  }
}

TEST(RouterTest, ShardCountRespected) {
  CommandRouter router(4, mycache::EvictionConfig{});
  EXPECT_EQ(router.shard_count(), 4u);
}

TEST(ShardWorkerTest, SubmitRunsAndStores) {
  ShardWorker worker(0, mycache::EvictionConfig{});
  worker.Start();

  std::string v;
  bool ok = false;
  std::atomic<bool> done{false};

  worker.Submit([&v, &ok, &done](Shard* shard) {
    shard->Set("k", "value");
    ok = shard->Get("k", &v);
    done.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(done.load());
  EXPECT_TRUE(ok);
  EXPECT_EQ(v, "value");

  worker.Stop();
}

TEST(RouterTest, RouteRoundTrip) {
  CommandRouter router(8, mycache::EvictionConfig{});
  router.Start();

  const std::string key = "routekey";

  // SET 与 GET 使用同一个 key，应路由到同一个 shard，从而读到刚写入的值。
  std::atomic<bool> set_done{false};
  router.Route(key, [&set_done, &key](Shard* s) {
    s->Set(key, "hello");
    set_done.store(true, std::memory_order_release);
  });

  std::string v;
  bool got = false;
  std::atomic<bool> get_done{false};
  router.Route(key, [&v, &got, &get_done, &key](Shard* s) {
    got = s->Get(key, &v);
    get_done.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 1000 && !(set_done.load() && get_done.load()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(set_done.load());
  EXPECT_TRUE(get_done.load());
  EXPECT_TRUE(got);
  EXPECT_EQ(v, "hello");

  router.Stop();
}
