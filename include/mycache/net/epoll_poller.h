#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <vector>

namespace mycache {

// epoll(7) 的轻量封装。所有注册的 fd 均使用边沿触发模式。
class EpollPoller {
 public:
  EpollPoller();
  ~EpollPoller();

  void Add(int fd, uint32_t events);
  void Mod(int fd, uint32_t events);
  void Del(int fd);

  // 返回就绪事件的个数（>= 0）。发生非 EINTR 错误时返回 -1。
  int Poll(int timeout_ms, std::vector<epoll_event>* out);

  int fd() const { return epoll_fd_; }

 private:
  int epoll_fd_;
  std::vector<epoll_event> events_;
};

}  // namespace mycache
