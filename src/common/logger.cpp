#include "mycache/common/logger.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <cstring>

namespace mycache {

namespace {
constexpr auto kFlushInterval = std::chrono::milliseconds(5);
}

LogLevel Logger::level_ = LogLevel::kInfo;
std::string Logger::file_;
FILE* Logger::fp_ = stderr;
std::mutex Logger::mutex_;
std::string Logger::pending_;
std::atomic<bool> Logger::running_{false};
std::thread Logger::flush_thread_;

void Logger::Init(LogLevel level, const std::string& file) {
  level_ = level;
  file_ = file;
  if (!file.empty()) {
    FILE* fp = std::fopen(file.c_str(), "a");
    fp_ = fp ? fp : stderr;
  } else {
    fp_ = stderr;
  }
  running_ = true;
  flush_thread_ = std::thread(&Logger::FlushLoop);
}

LogLevel Logger::level() { return level_; }

void Logger::Shutdown() {
  if (running_.exchange(false)) {
    if (flush_thread_.joinable()) flush_thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_.empty()) {
    std::fwrite(pending_.data(), 1, pending_.size(), fp_);
    pending_.clear();
  }
  std::fflush(fp_);
  if (fp_ != stderr) {
    std::fclose(fp_);
    fp_ = stderr;
  }
}

void Logger::Log(LogLevel lv, const char* file, int line, const char* fmt, ...) {
  if (static_cast<int>(lv) < static_cast<int>(level_)) return;

  char buf[2048];
  int n = 0;

  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  n += std::snprintf(buf + n, sizeof(buf) - n, "%02d-%02d %02d:%02d:%02d ",
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  n += std::snprintf(buf + n, sizeof(buf) - n, "[%ld] ", (long)::syscall(SYS_gettid));
  n += std::snprintf(buf + n, sizeof(buf) - n, "[%s] ", LevelName(lv));
  n += std::snprintf(buf + n, sizeof(buf) - n, "%s:%d ", file, line);

  va_list ap;
  va_start(ap, fmt);
  n += std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);

  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
  buf[n++] = '\n';
  buf[n] = '\0';

  // FATAL 或未初始化（单测等）：同步直写，保证消息立即可见、不依赖后台线程。
  if (lv == LogLevel::kFatal || !running_.load()) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fwrite(buf, 1, n, fp_);
    std::fflush(fp_);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.append(buf, n);
  }
}

const char* Logger::LevelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kFatal: return "FATAL";
  }
  return "?";
}

void Logger::FlushLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(kFlushInterval);
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.empty()) continue;
    std::fwrite(pending_.data(), 1, pending_.size(), fp_);  // 持锁写，避免与 append 交错
    std::fflush(fp_);
    pending_.clear();
  }
}

}  // namespace mycache
