#pragma once

#include <atomic>
#include <cstdint>

namespace mycache {

// 进程级统计计数（原子，跨线程安全）。供 INFO 输出命中率、淘汰/过期计数等。
class Metrics {
 public:
  static Metrics& Instance();

  void IncrHits() { hits_.fetch_add(1, std::memory_order_relaxed); }
  void IncrMisses() { misses_.fetch_add(1, std::memory_order_relaxed); }
  void IncrEvicted() { evicted_.fetch_add(1, std::memory_order_relaxed); }
  void IncrExpired() { expired_.fetch_add(1, std::memory_order_relaxed); }
  void IncrCommands() { commands_.fetch_add(1, std::memory_order_relaxed); }
  void IncrConnections() { connections_.fetch_add(1, std::memory_order_relaxed); }
  void IncrConnected() { connected_clients_.fetch_add(1, std::memory_order_relaxed); }
  void DecrConnected() { connected_clients_.fetch_sub(1, std::memory_order_relaxed); }

  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
  uint64_t evicted() const { return evicted_.load(std::memory_order_relaxed); }
  uint64_t expired() const { return expired_.load(std::memory_order_relaxed); }
  uint64_t commands() const { return commands_.load(std::memory_order_relaxed); }
  uint64_t connections() const { return connections_.load(std::memory_order_relaxed); }
  uint64_t connected_clients() const { return connected_clients_.load(std::memory_order_relaxed); }

  // 记录进程启动时刻（幂等），用于计算 uptime（基于单调钟）。
  void MarkStart();
  uint64_t UptimeSeconds() const;

 private:
  Metrics() = default;

  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> evicted_{0};
  std::atomic<uint64_t> expired_{0};
  std::atomic<uint64_t> commands_{0};
  std::atomic<uint64_t> connections_{0};
  std::atomic<uint64_t> connected_clients_{0};
  std::atomic<int64_t> start_ms_{0};
};

}  // namespace mycache
