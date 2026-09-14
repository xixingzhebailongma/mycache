#include "mycache/net/acceptor.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

#include "mycache/common/logger.h"
#include "mycache/net/socket_ops.h"

namespace mycache {

Acceptor::Acceptor(EventLoop* loop, const std::string& bind_ip, int port, int backlog)
    : loop_(loop), listen_fd_(-1) {
  std::string err;
  listen_fd_ = sockets::CreateListenSocket(bind_ip, port, backlog, &err);
  if (listen_fd_ < 0) {
    LOG_FATAL("%s", err.c_str());
    return;
  }
  channel_ = std::make_unique<Channel>(loop, listen_fd_);
  channel_->SetReadCallback([this] { HandleRead(); });
  channel_->EnableReading();
}

Acceptor::~Acceptor() {
  if (channel_) {
    channel_->DisableAll();
    channel_->Remove();
  }
  if (listen_fd_ >= 0) sockets::Close(listen_fd_);
}

void Acceptor::HandleRead() {
  while (true) {
    sockaddr_in peer{};
    socklen_t addrlen = sizeof peer;
    int connfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &addrlen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 已接受完
      if (errno == EINTR) continue;
      if (errno == EMFILE || errno == ENFILE) {
        LOG_WARN("accept() failed (fd exhausted): %s", strerror(errno));
        break;
      }
      LOG_ERROR("accept() error: %s", strerror(errno));
      break;
    }
    sockets::SetTcpNoDelay(connfd, true);
    std::string peer_str = sockets::PeerToString(peer);
    if (new_conn_cb_) new_conn_cb_(connfd, peer_str);
  }
}

}  // namespace mycache
