#include "mycache/net/event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "mycache/common/logger.h"

namespace mycache {

EventLoop::EventLoop()
    : poller_(),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      thread_id_(std::this_thread::get_id()) {
  if (wakeup_fd_ < 0) {
    LOG_FATAL("eventfd() failed: %s", strerror(errno));
  }
  wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fd_);
  wakeup_channel_->SetReadCallback([this] { HandleWakeup(); });
  wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
  wakeup_channel_->DisableAll();
  wakeup_channel_->Remove();
  ::close(wakeup_fd_);
}

void EventLoop::Loop() {
  thread_id_ = std::this_thread::get_id();
  looping_ = true;
  LOG_INFO("event loop %p starting", static_cast<void*>(this));

  std::vector<epoll_event> active;
  while (!quit_) {
    int n = poller_.Poll(-1, &active);
    for (int i = 0; i < n; ++i) {
      int fd = active[i].data.fd;
      auto it = channels_.find(fd);
      if (it == channels_.end()) continue;
      Channel* ch = it->second;
      ch->set_revents(active[i].events);
      ch->HandleEvent();
    }
    DoPendingFunctors();
  }

  looping_ = false;
  LOG_INFO("event loop %p quit", static_cast<void*>(this));
}

void EventLoop::Quit() {
  quit_ = true;
  if (!IsInLoopThread()) Wakeup();
}

void EventLoop::RunInLoop(Functor cb) {
  if (IsInLoopThread()) {
    cb();
  } else {
    QueueInLoop(std::move(cb));
  }
}

void EventLoop::QueueInLoop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_functors_.push_back(std::move(cb));
  }
  if (!IsInLoopThread() || calling_pending_) Wakeup();
}

void EventLoop::Wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeup_fd_, &one, sizeof one);
  (void)n;  // 返回 EAGAIN 表示已经处于唤醒状态。
}

void EventLoop::HandleWakeup() {
  uint64_t one;
  ssize_t n = ::read(wakeup_fd_, &one, sizeof one);
  (void)n;
}

void EventLoop::DoPendingFunctors() {
  std::vector<Functor> functors;
  calling_pending_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pending_functors_);
  }
  for (auto& f : functors) f();
  calling_pending_ = false;
}

void EventLoop::UpdateChannel(Channel* ch) {
  uint32_t events = ch->events();
  if (channels_.count(ch->fd()) == 0) {
    channels_[ch->fd()] = ch;
    poller_.Add(ch->fd(), events);
  } else {
    poller_.Mod(ch->fd(), events);
  }
}

void EventLoop::RemoveChannel(Channel* ch) {
  if (channels_.erase(ch->fd())) {
    poller_.Del(ch->fd());
  }
}

// ---- Channel ----

void Channel::Update() { loop_->UpdateChannel(this); }

void Channel::EnableReading() {
  events_ |= EPOLLIN;
  Update();
}
void Channel::DisableReading() {
  events_ &= ~EPOLLIN;
  Update();
}
void Channel::EnableWriting() {
  events_ |= EPOLLOUT;
  Update();
}
void Channel::DisableWriting() {
  events_ &= ~EPOLLOUT;
  Update();
}
void Channel::DisableAll() {
  events_ = 0;
  Update();
}
void Channel::Remove() { loop_->RemoveChannel(this); }

void Channel::HandleEvent() {
  std::shared_ptr<void> guard;
  if (tied_) {
    guard = tie_.lock();
    if (!guard) return;  // 拥有者对象已销毁
  }

  uint32_t events = revents_;
  if ((events & EPOLLHUP) && !(events & EPOLLIN)) {
    if (close_cb_) close_cb_();
    return;
  }
  if (events & EPOLLERR) {
    if (error_cb_) error_cb_();
  }
  if (events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
    if (read_cb_) read_cb_();
  }
  if (events & EPOLLOUT) {
    if (write_cb_) write_cb_();
  }
}

}  // namespace mycache
