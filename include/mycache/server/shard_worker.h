#pragma once

#include <functional>
#include <memory>
#include <thread>

#include "mycache/eviction/eviction_policy.h"
#include "mycache/net/event_loop.h"
#include "mycache/storage/db/shard.h"

namespace mycache {

// 一个 shard worker：拥有一个 Shard 存储，并在自己的线程中运行一个 EventLoop，
// 串行执行派发到它的命令任务；同时用 timerfd 驱动周期性的主动过期与内存兜底淘汰。
class ShardWorker {
 public:
  ShardWorker(int index, const EvictionConfig& eviction);
  ~ShardWorker();

  void Start();
  void Stop();

  int index() const { return index_; }
  Shard* shard() { return &shard_; }

  // 线程安全：把任务调度到本 shard 线程执行；fn 收到本 shard 的存储指针。
  void Submit(std::function<void(Shard*)> fn);

 private:
  void StartActiveExpireTimer();
  void OnTimer();

  int index_;
  Shard shard_;
  EventLoop loop_;
  std::thread thread_;
  int timer_fd_ = -1;
  std::unique_ptr<Channel> timer_channel_;
};

}  // namespace mycache
