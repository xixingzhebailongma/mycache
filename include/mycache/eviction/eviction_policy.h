#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mycache {

// 淘汰策略。与 Redis 的 maxmemory-policy 对应：
//   noeviction       — 超限时拒绝写入（返回 OOM 错误）
//   allkeys-lru      — 在所有 key 中采样，淘汰最久未访问的
//   allkeys-random   — 在所有 key 中采样，随机淘汰
//   volatile-lru     — 仅在设置了 TTL 的 key 中采样，淘汰最久未访问的
//   volatile-random  — 仅在设置了 TTL 的 key 中采样，随机淘汰
enum class EvictionPolicy {
  kNoEviction = 0,
  kAllkeysLru,
  kAllkeysRandom,
  kVolatileLru,
  kVolatileRandom,
};

// 每个 shard 的淘汰配置。maxmemory 是全局内存上限（字节，0 = 不限制）；
// shard 只在自己的写路径上、从自己的 key 里淘汰。
struct EvictionConfig {
  int64_t maxmemory = 0;
  int maxmemory_samples = 5;
  EvictionPolicy policy = EvictionPolicy::kNoEviction;
};

bool ParseEvictionPolicy(const std::string& s, EvictionPolicy* out);
const char* EvictionPolicyName(EvictionPolicy p);

}  // namespace mycache
