#include "mycache/net/connection.h"

#include <cerrno>
#include <cstring>

#include "mycache/common/logger.h"
#include "mycache/monitor/metrics.h"
#include "mycache/net/socket_ops.h"

namespace mycache {

// pending 字节上限（瞬时硬内存界）。正常混合 pipeline 峰值 ~几百 KB 到几 MB，
// 64 MiB 约两个数量级余量。若未来引入 value/bulk 长度上限，此值应随其走而非写死。
constexpr size_t kMaxPendingReplyBytes = 64ull << 20;

Connection::Connection(EventLoop* loop, int fd, std::string peer)
    : loop_(loop),
      fd_(fd),
      peer_(std::move(peer)),
      channel_(std::make_unique<Channel>(loop, fd)) {
  channel_->SetReadCallback([this] { HandleRead(); });
  channel_->SetWriteCallback([this] { HandleWrite(); });
  channel_->SetCloseCallback([this] { HandleClose(); });
  channel_->SetErrorCallback([this] { HandleError(); });
}

Connection::~Connection() { sockets::Close(fd_); }

void Connection::Establish() {
  channel_->Tie(shared_from_this());
  channel_->EnableReading();
}

void Connection::Send(uint64_t seq, const std::string& name, const std::string& data) {
  if (loop_->IsInLoopThread()) {
    SendInLoop(seq, name, data);
  } else {
    // 跨线程写回（如 shard worker -> IO reactor）：捕获 shared_from_this() 以
    // 保证连接在排队的写回调执行前存活。
    auto self = shared_from_this();
    loop_->RunInLoop([self, seq, name, data] { self->SendInLoop(seq, name, data); });
  }
}

void Connection::SendFatal(const std::string& data) {
  if (closing_) return;
  output_.append(data);
  if (!channel_->writing()) channel_->EnableWriting();
}

void Connection::SendInLoop(uint64_t seq, const std::string& name, const std::string& data) {
  if (closing_) return;

  // 按命令解析序号重排序：只有 seq 恰为下一个应发出的序号才直接入输出缓冲；
  // 乱序到达的先暂存，等前面的到位后按序补发。
  if (seq == next_reply_seq_) {
    output_.append(data);
    ++next_reply_seq_;
    for (;;) {
      auto it = pending_replies_.find(next_reply_seq_);
      if (it == pending_replies_.end()) break;
      output_.append(it->second.data);
      pending_bytes_ -= it->second.data.size() + it->second.name.size();
      pending_replies_.erase(it);
      ++next_reply_seq_;
    }
    if (!channel_->writing()) channel_->EnableWriting();
  } else if (seq > next_reply_seq_) {
    const size_t sz = data.size() + name.size();
    if (pending_bytes_ + sz > kMaxPendingReplyBytes) {
      LOG_WARN("fd %d pending replies exceed %zu bytes (stuck on '%s'), closing",
               fd_, kMaxPendingReplyBytes, name.c_str());
      HandleClose();
      return;
    }
    pending_replies_[seq] = PendingReply{name, data};
    pending_bytes_ += sz;
  } else {
    LOG_ERROR("stale reply for command '%s' seq %llu (next %llu) on fd %d, dropping",
              name.c_str(), static_cast<unsigned long long>(seq),
              static_cast<unsigned long long>(next_reply_seq_), fd_);
    Metrics::Instance().IncrStaleReplies();
  }
}

void Connection::Shutdown() {
  if (loop_->IsInLoopThread()) {
    ShutdownInLoop();
  } else {
    auto self = shared_from_this();
    loop_->RunInLoop([self] { self->ShutdownInLoop(); });
  }
}

void Connection::ShutdownInLoop() {
  if (closing_) return;
  if (output_.readable() > 0) {
    closing_ = true;  // 待发送的输出刷出后再关闭
  } else {
    HandleClose();
  }
}

void Connection::HandleRead() {
  int saved_errno = 0;
  while (true) {
    ssize_t n = input_.readFd(fd_, &saved_errno);
    if (n > 0) continue;
    if (n == 0) {
      HandleClose();
      return;
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) break;  // 已读空
    LOG_ERROR("read error on fd %d: %s", fd_, strerror(saved_errno));
    HandleError();
    return;
  }
  if (message_cb_) message_cb_(shared_from_this(), &input_);
}

void Connection::HandleWrite() {
  if (!channel_->writing()) return;

  int saved_errno = 0;
  while (output_.readable() > 0) {
    ssize_t n = output_.writeFd(fd_, &saved_errno);
    if (n > 0) continue;
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) break;  // 会阻塞
    LOG_ERROR("write error on fd %d: %s", fd_, strerror(saved_errno));
    HandleError();
    return;
  }

  if (output_.readable() == 0) {
    channel_->DisableWriting();
    if (write_complete_cb_) write_complete_cb_(shared_from_this());
    if (closing_) HandleClose();
  }
}

void Connection::HandleClose() {
  if (closing_) return;
  closing_ = true;
  // 在析构函数关闭 fd 之前，先将其从 epoll 中移除（EPOLL_CTL_DEL），
  // 否则复用的 fd 编号会与残留的注册项发生冲突。
  channel_->Remove();
  if (close_cb_) close_cb_(shared_from_this());
}

void Connection::HandleError() { HandleClose(); }

}  // namespace mycache
