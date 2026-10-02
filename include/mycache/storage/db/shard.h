#pragma once

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "mycache/eviction/eviction_policy.h"
#include "mycache/storage/dict/dict.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

struct Object;

// 单个 shard 的存储：独立的 dict（sds -> Object*）+ expire dict（sds -> 到期
// 时间戳），并支持基于 dict 的 hash 值与 TTL。非线程安全，仅由其归属的
// ShardWorker 线程访问，因此无需加锁。
//
// 内存治理：每个 shard 持有全局 maxmemory 水位，在自己的写路径上检查
// used_memory() 是否超限，并仅从自己哈希分配到的 key 里按策略淘汰——全局水位
// 与「按 shard 均分」的等价性见 docs/eviction.md。
class Shard {
 public:
  Shard();
  ~Shard();

  Shard(const Shard&) = delete;
  Shard& operator=(const Shard&) = delete;

  // ---- 内存治理 ----
  void ConfigureEviction(const EvictionConfig& cfg);
  // 写入前调用：若内存超限，按策略淘汰本 shard 的 key。返回 false 表示仍超限
  // 且无法继续（调用方应回 OOM 错误）。
  bool EvictIfNeeded();
  // 按策略淘汰单个 key（内部被 EvictIfNeeded 调用；公开以便测试验证淘汰顺序）。
  bool EvictOne();
  // 按时间预算推进 rehash（对齐 Redis databasesCron 的 incrementallyRehash）：
  // 空闲时也保证进行中的 rehash 能收敛，避免 ht[0]/ht[1] 双表常驻。仅由其
  // 归属的 ShardWorker 线程在定时器里调用。
  void IncrementalRehash(int ms);

  // ---- string ----
  bool Get(const std::string& key, std::string* out);
  void Set(const std::string& key, const std::string& value);
  int Del(const std::vector<std::string>& keys);
  bool Exists(const std::string& key);
  std::string Type(const std::string& key);

  // ---- hash ----
  int HSet(const std::string& key, const std::string& field, const std::string& value);
  bool HGet(const std::string& key, const std::string& field, std::string* out);
  int HDel(const std::string& key, const std::vector<std::string>& fields);
  std::vector<std::pair<std::string, std::string>> HGetAll(const std::string& key);

  // ---- list ----
  int LPush(const std::string& key, const std::vector<std::string>& values);
  int RPush(const std::string& key, const std::vector<std::string>& values);
  bool LPop(const std::string& key, std::string* out);
  bool RPop(const std::string& key, std::string* out);
  std::vector<std::string> LRange(const std::string& key, long start, long stop);
  size_t LLen(const std::string& key);

  // ---- TTL ----
  bool ExpireAt(const std::string& key, int64_t when_ms);  // 绝对到期时间戳(UnixMs)；key 存在返回 true
  bool ExpireMs(const std::string& key, int64_t ms);  // 相对 TTL；key 存在返回 true
  int64_t TtlMs(const std::string& key);              // -2 不存在，-1 永不过期，否则剩余 ms

  // 主动过期（简化版）：随机采样若干桶，删除已到期 key。
  void ActiveExpireCycle();

  size_t Size() const;         // 当前 key 数（原子，跨线程安全）
  size_t ExpireCount() const;  // 带 TTL 的 key 数（原子，跨线程安全）

  // 清空本 shard 全部 key（含 TTL），供运行时 LOAD 重载前的整库清空。
  // 必须在持有 GlobalSnapshotGate 独占锁时调用（无并发 worker 访问）。
  // 注意：Dict::Clear() 只释放条目并复位 ht/rehashidx，【不会】动 key_count_/expire_count_
  // 这两个 Shard 手动维护的原子计数；因此这里必须手动 store(0)，否则 Size()/ExpireCount()
  // 残留旧值。清空后 dict 处于可立即写入状态（Clear 已复位 rehashidx 与两表）。
  // 跨 shard 不保证原子：依赖 Dict::Clear() 不抛异常；若某 shard 的 FlushAll 中途抛异常，
  // 会留下「部分 shard 已清空、部分还是旧数据」的中间态——这是 DEBUG RELOAD 风格的取舍。
  void FlushAll();

  // ---- 快照遍历（无锁、无分配，供 fork 出的子进程做 AOF rewrite / RDB 快照）----
  // 对 dict_ 中每个条目回调 visit(key, key_len, val, ttl_ms, ud)。val 为 Object*；
  // ttl_ms 为绝对到期时间戳（UnixMs），无 TTL 时为 -1。仅只读，不推进 rehash、
  // 不修改任何状态，因此可在 fork 出的子进程中安全遍历 COW 快照。
  using SnapshotVisitor = void (*)(const char* key, size_t key_len,
                                   const Object* val, int64_t ttl_ms, void* ud);
  void ScanSnapshot(SnapshotVisitor visit, void* ud) const;

 private:
  bool DeleteKey(sds key);         // 从 dict_ 与 expires_ 同时删除；返回 key 是否存在
  bool ExpireIfNeeded(sds key);    // 惰性删除：已到期则删除并返回 true
  Object* LookupValue(sds key);    // 惰性删除后的查找，刷新 LRU

  std::vector<DictEntry*> SampleCandidates();
  sds PickVictim(const std::vector<DictEntry*>& candidates);

  Dict dict_;     // sds key -> Object*
  Dict expires_;  // sds key -> int64_t 到期时间戳(ms)，存于 void*
  std::mt19937_64 rng_;

  EvictionConfig eviction_;
  std::atomic<size_t> key_count_{0};
  std::atomic<size_t> expire_count_{0};
};

}  // namespace mycache
