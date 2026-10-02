#pragma once

#include "mycache/io/io_engine.h"

namespace mycache {

// 直接封装 libc 的 POSIX 文件 I/O（open/write/pread/fsync/ftruncate/close）。
class PosixIO : public IOEngine {
 public:
  int Open(const std::string& path, int flags, int mode) override;
  ssize_t Write(int fd, const void* buf, size_t len) override;
  ssize_t Writev(int fd, const iovec* iov, int iovcnt) override;
  ssize_t WritevFsync(int fd, const iovec* iov, int iovcnt) override;
  ssize_t Pread(int fd, void* buf, size_t len, off_t off) override;
  int Fsync(int fd) override;
  int Ftruncate(int fd, off_t len) override;
  int Close(int fd) override;

  const char* Name() const override { return "posix"; }
};

}  // namespace mycache
