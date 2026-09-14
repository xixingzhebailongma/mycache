#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mycache/net/buffer.h"
#include "mycache/net/event_loop.h"

namespace mycache {

// 由 IO 反应器持有的 TCP 连接。这里处理读（边沿触发：直到 EAGAIN）和写
// （边沿触发：直到 EAGAIN）；应用逻辑通过回调接入。
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using MessageCallback = std::function<void(const std::shared_ptr<Connection>&, Buffer*)>;
  using CloseCallback = std::function<void(const std::shared_ptr<Connection>&)>;
  using WriteCompleteCallback = std::function<void(const std::shared_ptr<Connection>&)>;

  Connection(EventLoop* loop, int fd, std::string peer);
  ~Connection();

  // 注册通道并开始读取。必须在循环线程上执行。
  void Establish();

  void Send(const std::string& data);
  // 优雅关闭：先将输出缓冲区刷出，再关闭连接。
  void Shutdown();

  EventLoop* loop() const { return loop_; }
  int fd() const { return fd_; }
  const std::string& peer() const { return peer_; }
  Buffer* input() { return &input_; }
  Buffer* output() { return &output_; }

  void SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
  void SetCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }
  void SetWriteCompleteCallback(WriteCompleteCallback cb) { write_complete_cb_ = std::move(cb); }

  void HandleRead();
  void HandleWrite();
  void HandleClose();
  void HandleError();

 private:
  void SendInLoop(const std::string& data);
  void ShutdownInLoop();

  EventLoop* loop_;
  int fd_;
  std::string peer_;
  std::unique_ptr<Channel> channel_;
  Buffer input_;
  Buffer output_;

  MessageCallback message_cb_;
  CloseCallback close_cb_;
  WriteCompleteCallback write_complete_cb_;
  bool closing_ = false;
};

}  // namespace mycache
