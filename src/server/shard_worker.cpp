#include "mycache/server/shard_worker.h"

#include <cerrno>
#include <sys/timerfd.h>
#include <unistd.h>

#include "mycache/common/logger.h"
#include "mycache/eviction/lru.h"

namespace mycache {

namespace {
constexpr long kActiveExpireIntervalMs = 100;
}  // namespace

ShardWorker::ShardWorker(int index, const EvictionConfig& eviction) : index_(index) {
  shard_.ConfigureEviction(eviction);
}

ShardWorker::~ShardWorker() { Stop(); }

void ShardWorker::Start() {
  StartActiveExpireTimer();
  thread_ = std::thread([this] { loop_.Loop(); });
}

void ShardWorker::Stop() {
  if (thread_.joinable()) {
    loop_.Quit();
    thread_.join();
  }
  if (timer_channel_) {
    timer_channel_->DisableAll();
    timer_channel_->Remove();
    timer_channel_.reset();
  }
  if (timer_fd_ >= 0) {
    ::close(timer_fd_);
    timer_fd_ = -1;
  }
}

void ShardWorker::Submit(std::function<void(Shard*)> fn) {
  Shard* s = &shard_;
  loop_.RunInLoop([s, fn = std::move(fn)] { fn(s); });
}

void ShardWorker::StartActiveExpireTimer() {
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ < 0) {
    LOG_ERROR("timerfd_create failed: %d", errno);
    return;
  }
  itimerspec its{};
  its.it_interval.tv_nsec = kActiveExpireIntervalMs * 1000 * 1000;
  its.it_value.tv_nsec = kActiveExpireIntervalMs * 1000 * 1000;
  ::timerfd_settime(timer_fd_, 0, &its, nullptr);

  timer_channel_ = std::make_unique<Channel>(&loop_, timer_fd_);
  timer_channel_->SetReadCallback([this] { OnTimer(); });
  timer_channel_->EnableReading();
}

void ShardWorker::OnTimer() {
  uint64_t expirations = 0;
  ssize_t n = ::read(timer_fd_, &expirations, sizeof expirations);
  (void)n;
  RefreshLruClock();           // 周期刷新 LRU 时钟（~100ms，与 Redis serverCron 一致）
  shard_.ActiveExpireCycle();
  shard_.EvictIfNeeded();      // 超限时的后台兜底淘汰（即便之后没有新写入）
}

}  // namespace mycache
