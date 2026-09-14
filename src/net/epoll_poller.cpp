#include "mycache/net/epoll_poller.h"

#include <cerrno>
#include <unistd.h>

namespace mycache {

namespace {
uint32_t WithET(uint32_t events) {
  uint32_t e = events | EPOLLET;
  // 仅在关注输入时才请求 EPOLLRDHUP，这样完全禁用的通道会保持安静。
  if (events & EPOLLIN) e |= EPOLLRDHUP;
  return e;
}
}  // namespace

EpollPoller::EpollPoller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
  events_.resize(1024);
}

EpollPoller::~EpollPoller() { ::close(epoll_fd_); }

void EpollPoller::Add(int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = WithET(events);
  ev.data.fd = fd;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void EpollPoller::Mod(int fd, uint32_t events) {
  epoll_event ev{};
  ev.events = WithET(events);
  ev.data.fd = fd;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollPoller::Del(int fd) {
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int EpollPoller::Poll(int timeout_ms, std::vector<epoll_event>* out) {
  int n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
  if (n < 0 && errno == EINTR) {
    n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
  }
  if (n > 0) {
    out->assign(events_.begin(), events_.begin() + n);
  } else if (n == 0) {
    out->clear();
  }
  return n;
}

}  // namespace mycache
