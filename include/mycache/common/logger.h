#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace mycache {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError, kFatal };

// 极简的线程安全日志器，写入 stderr（默认）或文件。
class Logger {
 public:
  static void Init(LogLevel level, const std::string& file = "") {
    level_ = level;
    file_ = file;
    if (!file.empty()) {
      FILE* fp = std::fopen(file.c_str(), "a");
      fp_ = fp ? fp : stderr;
    } else {
      fp_ = stderr;
    }
  }

  static LogLevel level() { return level_; }

  static void Log(LogLevel lv, const char* file, int line, const char* fmt, ...) {
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

    std::lock_guard<std::mutex> lock(mutex_);
    std::fwrite(buf, 1, n, fp_);
    std::fflush(fp_);
  }

 private:
  static const char* LevelName(LogLevel lv) {
    switch (lv) {
      case LogLevel::kDebug: return "DEBUG";
      case LogLevel::kInfo:  return "INFO";
      case LogLevel::kWarn:  return "WARN";
      case LogLevel::kError: return "ERROR";
      case LogLevel::kFatal: return "FATAL";
    }
    return "?";
  }

  inline static LogLevel level_ = LogLevel::kInfo;
  inline static std::string file_;
  inline static FILE* fp_ = stderr;
  inline static std::mutex mutex_;
};

#define LOG_DEBUG(...) ::mycache::Logger::Log(::mycache::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::mycache::Logger::Log(::mycache::LogLevel::kInfo,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::mycache::Logger::Log(::mycache::LogLevel::kWarn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::mycache::Logger::Log(::mycache::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) ::mycache::Logger::Log(::mycache::LogLevel::kFatal, __FILE__, __LINE__, __VA_ARGS__)

}  // namespace mycache
