#include "mycache/net/connection.h"

#include <cerrno>
#include <cstring>

#include "mycache/common/logger.h"
#include "mycache/net/socket_ops.h"

namespace mycache {

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

void Connection::Send(const std::string& data) {
  if (loop_->IsInLoopThread()) {
    SendInLoop(data);
  } else {
    // 跨线程写回（如 shard worker -> IO reactor）：捕获 shared_from_this() 以
    // 保证连接在排队的写回调执行前存活。
    auto self = shared_from_this();
    loop_->RunInLoop([self, data] { self->SendInLoop(data); });
  }
}

void Connection::SendInLoop(const std::string& data) {
  if (closing_) return;
  output_.append(data);
  if (!channel_->writing()) channel_->EnableWriting();
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
