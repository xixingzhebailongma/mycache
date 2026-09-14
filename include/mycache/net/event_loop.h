#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mycache/net/epoll_poller.h"

namespace mycache {

class EventLoop;

// 将 fd 与其关注的事件、以及事件触发时调用的回调关联起来。
// 由注册它的对象（Connection、Acceptor 等）持有。
class Channel {
 public:
  using Callback = std::function<void()>;

  Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

  int fd() const { return fd_; }
  uint32_t events() const { return events_; }
  void set_revents(uint32_t revents) { revents_ = revents; }

  bool reading() const { return events_ & EPOLLIN; }
  bool writing() const { return events_ & EPOLLOUT; }

  void EnableReading();
  void DisableReading();
  void EnableWriting();
  void DisableWriting();
  void DisableAll();
  void Remove();

  void SetReadCallback(Callback cb) { read_cb_ = std::move(cb); }
  void SetWriteCallback(Callback cb) { write_cb_ = std::move(cb); }
  void SetCloseCallback(Callback cb) { close_cb_ = std::move(cb); }
  void SetErrorCallback(Callback cb) { error_cb_ = std::move(cb); }

  // 在处理事件期间保持拥有者对象存活。
  void Tie(const std::shared_ptr<void>& obj) { tie_ = obj; tied_ = true; }

  void HandleEvent();

 private:
  void Update();

  EventLoop* loop_;
  const int fd_;
  uint32_t events_ = 0;
  uint32_t revents_ = 0;
  Callback read_cb_;
  Callback write_cb_;
  Callback close_cb_;
  Callback error_cb_;
  std::weak_ptr<void> tie_;
  bool tied_ = false;
};

// 一个线程 + 一个 epoll 实例 + 一个待执行回调队列。已注册的通道在循环线程上
// 更新，事件也在该线程上分发。
class EventLoop {
 public:
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();

  void Loop();
  void Quit();
  bool IsInLoopThread() const { return std::this_thread::get_id() == thread_id_; }

  void RunInLoop(Functor cb);
  void QueueInLoop(Functor cb);

  void UpdateChannel(Channel* ch);
  void RemoveChannel(Channel* ch);

 private:
  void Wakeup();
  void HandleWakeup();
  void DoPendingFunctors();

  EpollPoller poller_;
  int wakeup_fd_;
  std::unique_ptr<Channel> wakeup_channel_;
  bool looping_ = false;
  std::atomic<bool> quit_{false};
  std::thread::id thread_id_;
  std::mutex mutex_;
  std::vector<Functor> pending_functors_;
  bool calling_pending_ = false;
  std::unordered_map<int, Channel*> channels_;
};

}  // namespace mycache
