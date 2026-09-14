#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mycache/net/event_loop.h"

namespace mycache {

// 在主反应器上持有监听套接字；接受连接并把每个连接交给回调（通常是 IO 反应器）。
class Acceptor {
 public:
  using NewConnectionCallback = std::function<void(int fd, const std::string& peer)>;

  Acceptor(EventLoop* loop, const std::string& bind_ip, int port, int backlog);
  ~Acceptor();

  bool valid() const { return listen_fd_ >= 0; }

  void SetNewConnectionCallback(NewConnectionCallback cb) { new_conn_cb_ = std::move(cb); }

 private:
  void HandleRead();

  EventLoop* loop_;
  int listen_fd_;
  std::unique_ptr<Channel> channel_;
  NewConnectionCallback new_conn_cb_;
};

}  // namespace mycache
