#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "mycache/net/buffer.h"
#include "mycache/net/event_loop.h"
#include "mycache/protocol/resp_parser.h"

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

  // 分配下一条命令的解析序号（仅 DispatchCommand 在解析线程按命令序调用）。
  uint64_t NextCmdSeq() { return next_cmd_seq_++; }

  // 有序发送回复：seq=命令解析序号，name=命令名（仅诊断用）。跨线程安全。
  void Send(uint64_t seq, const std::string& name, const std::string& data);

  // 致命协议错误：仅 loop 线程调用，绕过重排序缓冲直接入 output_。不参与 seq/
  // pending 计数，也不受 pending 字节 cap 约束（连接即将 Shutdown，无需限制）。
  void SendFatal(const std::string& data);
  // 优雅关闭：先将输出缓冲区刷出，再关闭连接。
  void Shutdown();

  EventLoop* loop() const { return loop_; }
  int fd() const { return fd_; }
  const std::string& peer() const { return peer_; }
  Buffer* input() { return &input_; }
  Buffer* output() { return &output_; }
  RespParser& parser() { return parser_; }

  void SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
  void SetCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }
  void SetWriteCompleteCallback(WriteCompleteCallback cb) { write_complete_cb_ = std::move(cb); }

  void HandleRead();
  void HandleWrite();
  void HandleClose();
  void HandleError();

 private:
  void SendInLoop(uint64_t seq, const std::string& name, const std::string& data);
  void ShutdownInLoop();

  EventLoop* loop_;
  int fd_;
  std::string peer_;
  std::unique_ptr<Channel> channel_;
  Buffer input_;
  Buffer output_;
  RespParser parser_;  // 每连接一份，跨读续接（消除半包重扫）

  MessageCallback message_cb_;
  CloseCallback close_cb_;
  WriteCompleteCallback write_complete_cb_;
  bool closing_ = false;

  // 回复有序性：next_cmd_seq_ 仅解析线程写；next_reply_seq_/pending_replies_/
  // pending_bytes_ 仅 loop 线程（SendInLoop）读写，无锁安全。
  uint64_t next_cmd_seq_ = 0;
  uint64_t next_reply_seq_ = 0;
  struct PendingReply { std::string name; std::string data; };
  std::map<uint64_t, PendingReply> pending_replies_;
  size_t pending_bytes_ = 0;
};

}  // namespace mycache
