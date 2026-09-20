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

// 是否允许共享整数对象（对齐 Redis MAXMEMORY_FLAG_NO_SHARED_INTEGERS）。
// LRU 淘汰依赖每个对象独立的 lru 时间戳；共享对象只有一份 lru，会破坏采样淘汰，
// 因此配置了 LRU 策略时关闭共享，其余（noeviction / random）允许。
//
// 注意：共享开关随淘汰策略在**启动期一次性确定**，运行期不可切换——一旦共享对象已写入
// dict，value 就是共享指针，切换回 LRU 也无法让已存对象重新拥有独立 lru（除非 O(N) 全量
// 重建）。若未来支持 CONFIG SET 动态改淘汰策略，必须同步处理已存储的共享对象，禁止简单
// 地在运行期开关共享。
bool AllowSharedIntegers(const EvictionConfig& ec);

bool ParseEvictionPolicy(const std::string& s, EvictionPolicy* out);
const char* EvictionPolicyName(EvictionPolicy p);

}  // namespace mycache
