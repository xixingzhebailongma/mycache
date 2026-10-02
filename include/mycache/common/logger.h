#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace mycache {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError, kFatal };

// 线程安全日志器，写入 stderr（默认）或文件。
//
// 非 FATAL 日志异步落盘：调用线程只把格式化好的字节追加到 pending_ 缓冲，由后台线程
// 定时（约 5ms）批量 fwrite + 一次 fflush，避免逐行 fflush 造成的一行一个 write(2)
// 系统调用。FATAL 日志仍同步直写，保证 abort/return 前消息一定落盘。
//
// 注意：因为持有后台线程，fork() 后子进程只剩调用线程存活。子进程（AOF rewrite /
// RDB bgsave）**不得调用 LOG_***，否则会因 mutex_ 状态不一致而死锁或丢日志。
// 当前子进程实现（RewriteAofChild / WriteRdbChild）只做无分配操作并 _exit，不打日志。
class Logger {
 public:
  static void Init(LogLevel level, const std::string& file = "");
  static LogLevel level();

  // 优雅关闭：停掉后台线程、排空残留缓冲并 flush（关闭文件则 fclose）。幂等。
  static void Shutdown();

  static void Log(LogLevel lv, const char* file, int line, const char* fmt, ...);

 private:
  static const char* LevelName(LogLevel lv);
  static void FlushLoop();

  static LogLevel level_;
  static std::string file_;
  static FILE* fp_;
  static std::mutex mutex_;              // 保护 pending_ 与同步直写
  static std::string pending_;           // 待落盘的日志字节（mutex_ 保护）
  static std::atomic<bool> running_;
  static std::thread flush_thread_;
};

#define LOG_DEBUG(...) ::mycache::Logger::Log(::mycache::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::mycache::Logger::Log(::mycache::LogLevel::kInfo,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::mycache::Logger::Log(::mycache::LogLevel::kWarn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::mycache::Logger::Log(::mycache::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) ::mycache::Logger::Log(::mycache::LogLevel::kFatal, __FILE__, __LINE__, __VA_ARGS__)

}  // namespace mycache
