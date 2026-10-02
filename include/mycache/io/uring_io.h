#pragma once

#include "mycache/io/io_engine.h"
#include "mycache/io/posix_io.h"

#ifdef MYCACHE_HAVE_IO_URING

#include <liburing.h>

namespace mycache {

// io_uring 引擎：write / fsync 走 io_uring（同步提交 SQE + 等待 CQE），其余
// open/pread/ftruncate/close 复用 PosixIO。这里的同步 submit/wait 主要用于演示
// io_uring 的提交/完成模型与压测对比；真正的收益来自将来把多个 SQE 批提交（异步）。
class UringIO : public IOEngine {
 public:
  UringIO();
  ~UringIO() override;

  bool valid() const { return ok_; }

  int Open(const std::string& path, int flags, int mode) override {
    return posix_.Open(path, flags, mode);
  }
  ssize_t Write(int fd, const void* buf, size_t len) override;
  // 普通 writev（无 fsync）暂委托 posix；组提交的 writev+fsync 见 WritevFsync。
  ssize_t Writev(int fd, const iovec* iov, int iovcnt) override {
    return posix_.Writev(fd, iov, iovcnt);
  }
  // writev + fsync 一次完成（linked SQE：WRITEV -> FSYNC，单次 submit 等两个 CQE）。
  // 本函数仅由 GroupCommitLoop（always 模式）单线程调用，ring 无并发访问。
  // everysec/no 模式下 ring 仍由 shard 线程 inline io_->Write 访问，为既有行为，本次不改。
  ssize_t WritevFsync(int fd, const iovec* iov, int iovcnt) override;
  ssize_t Pread(int fd, void* buf, size_t len, off_t off) override {
    return posix_.Pread(fd, buf, len, off);
  }
  int Fsync(int fd) override;
  int Ftruncate(int fd, off_t len) override { return posix_.Ftruncate(fd, len); }
  int Close(int fd) override { return posix_.Close(fd); }

  const char* Name() const override { return "io_uring"; }

 private:
  io_uring ring_;
  bool ok_ = false;
  PosixIO posix_;
};

}  // namespace mycache

#endif  // MYCACHE_HAVE_IO_URING
