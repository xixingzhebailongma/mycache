#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

#include "mycache/net/connection.h"
#include "mycache/net/event_loop.h"

namespace mycache {

// IO 反应器在自己的线程中运行一个 EventLoop，并持有注册在它上面的连接。
class IOReactor {
 public:
  IOReactor() = default;
  ~IOReactor();

  void Start();
  void Stop();

  EventLoop* loop() { return &loop_; }

  // 线程安全：将连接的创建调度到该反应器的循环线程上执行。
  void AddConnection(int fd, const std::string& peer, Connection::MessageCallback msg_cb);

  size_t ConnectionCount() const { return connection_count_.load(); }

 private:
  EventLoop loop_;
  std::thread thread_;
  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  std::atomic<size_t> connection_count_{0};
};

}  // namespace mycache
