// 百万级 key 内存引擎压测：直接驱动 Keyspace（绕过网络），
// 验证 dict / 渐进 rehash / 对象编码 在百万规模下的正确性与内存占用。
//
// 用法：mycache_stress [nkeys]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mycache/storage/db/keyspace.h"
#include "mycache/storage/memory/allocator.h"
#include "mycache/storage/memory/memory_stats.h"

using mycache::Keyspace;

namespace {

// 与压测写入逻辑一致的期望值生成，用于 GET 校验。
std::string ExpectValue(size_t i) {
  if (i % 2 == 0) return std::to_string(i);             // int 编码
  return "value:" + std::to_string(i);                  // string 编码
}

}  // namespace

int main(int argc, char* argv[]) {
  size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;

  Keyspace ks;

  // SET：写入 N 个 key，值覆盖 int / string 两种编码。
  auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i < N; ++i) {
    ks.Set("key:" + std::to_string(i), ExpectValue(i));
  }
  auto t1 = std::chrono::steady_clock::now();

  // 抽样 GET：验证读写正确（覆盖 rehash 期间）。
  size_t mismatches = 0;
  size_t sampled = 0;
  for (size_t i = 0; i < N; i += 997) {
    std::string key = "key:" + std::to_string(i);
    std::string got;
    if (!ks.Get(key, &got) || got != ExpectValue(i)) ++mismatches;
    ++sampled;
  }
  auto t2 = std::chrono::steady_clock::now();

  double set_sec = std::chrono::duration<double>(t1 - t0).count();
  double get_sec = std::chrono::duration<double>(t2 - t1).count();

  std::printf("keys:            %zu\n", N);
  std::printf("keyspace size:   %zu\n", ks.Size());
  std::printf("SET:             %.2f s (%.0f ops/s)\n", set_sec, N / set_sec);
  std::printf("sampled GET:     %zu in %.3f s\n", sampled, get_sec);
  std::printf("rehashing now:   %s\n", ks.Rehashing() ? "yes" : "no");
  std::printf("buckets:         %zu\n", ks.BucketCount());
  std::printf("%s", mycache::MemoryStats::Instance().Summary().c_str());
  std::printf("mismatches:      %zu\n", mismatches);

  if (ks.Size() != N || mismatches != 0) {
    std::fprintf(stderr, "STRESS FAILED\n");
    return 1;
  }
  std::printf("STRESS OK\n");
  return 0;
}
