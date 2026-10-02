#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/uio.h>

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
  // 一次写入多个不连续 buffer（AOF 组提交攒批用）。语义同 Write：写满才返回（失败 -1）。
  virtual ssize_t Writev(int fd, const iovec* iov, int iovcnt) = 0;
  // 一次 writev + fsync（组提交的批量落盘原语）。语义同 Writev：写满并 fsync 成功才
  // 返回，返回写入总字节数（失败 -1）。posix 顺序 writev 后 fsync；io_uring 用
  // linked SQE（WRITEV -> FSYNC）单次 submit。linked SQE 下若 WRITEV 短写，link 的
  // FSYNC 仍会触发——本轮得到的是「部分数据已 fsync」的中间状态，剩余字节由下一轮
  // WRITEV+FSYNC 补齐。AOF 写普通文件几乎不短写，代价可接受。
  virtual ssize_t WritevFsync(int fd, const iovec* iov, int iovcnt) = 0;
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
