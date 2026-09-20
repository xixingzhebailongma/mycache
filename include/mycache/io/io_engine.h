#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <sys/types.h>

namespace mycache {

// 抽象 AOF/RDB 的热数据 I/O 路径：open / write / pread / fsync / ftruncate / close。
// rename 不是 io_uring 热点，仍由调用方用普通 syscall 完成。
//
// Write/Pread 内部循环处理 EINTR 与部分读写，语义上等价于"写满/读满"：Write 返回
// 写入总字节数（失败 -1），Pread 返回读到的字节数（失败 -1，读不满则返回实际数）。
class IOEngine {
 public:
  virtual ~IOEngine() = default;

  virtual int Open(const std::string& path, int flags, int mode) = 0;
  virtual ssize_t Write(int fd, const void* buf, size_t len) = 0;
  virtual ssize_t Pread(int fd, void* buf, size_t len, off_t off) = 0;
  virtual int Fsync(int fd) = 0;
  virtual int Ftruncate(int fd, off_t len) = 0;
  virtual int Close(int fd) = 0;

  virtual const char* Name() const = 0;
};

// 工厂：按名字 "posix" / "io_uring" 创建引擎。io_uring 未编译或初始化失败时回退
// posix 并告警。
std::unique_ptr<IOEngine> CreateIOEngine(const std::string& name);

}  // namespace mycache
