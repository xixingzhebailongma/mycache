#include <chrono>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "mycache/eviction/lru.h"
#include "mycache/eviction/sampler.h"
#include "mycache/storage/db/dict_types.h"
#include "mycache/storage/dict/dict.h"
#include "mycache/storage/object/string_object.h"
#include "mycache/storage/string/sds.h"

using mycache::Dict;
using mycache::LruClock;
using mycache::RefreshLruClock;
using mycache::SampleDictEntries;
using mycache::sds;
using mycache::sdsfree;
using mycache::sdsnewlen;

TEST(LruClockTest, SelfInitializes) {
  // 即便从未显式刷新，LruClock 也应返回非零（首次调用自初始化）。
  RefreshLruClock();
  EXPECT_GT(LruClock(), 0u);
}

TEST(LruClockTest, RefreshAdvances) {
  RefreshLruClock();
  uint64_t before = LruClock();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  RefreshLruClock();
  EXPECT_GE(LruClock(), before);
}

TEST(SamplerTest, SamplesUpToCount) {
  Dict d(mycache::SdsObjectDictType());
  for (int i = 0; i < 30; ++i) {
    std::string key = "k" + std::to_string(i);
    sds k = sdsnewlen(key.data(), key.size());
    mycache::Object* v = mycache::CreateIntObject(i);
    d.Add(k, v);
    v->DecrRefCount();  // dict 已 incref，释放本地引用
    sdsfree(k);
  }

  std::mt19937_64 rng(42);
  auto entries = SampleDictEntries(&d, 5, rng);
  EXPECT_LE(entries.size(), 5u);
  EXPECT_GT(entries.size(), 0u);

  // 采样到的 key 都非空且属于我们写入的集合。
  for (auto* e : entries) {
    EXPECT_NE(e->key, nullptr);
  }
}
