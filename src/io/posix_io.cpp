#include "mycache/io/posix_io.h"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace mycache {

int PosixIO::Open(const std::string& path, int flags, int mode) {
  return ::open(path.c_str(), flags, mode);
}

ssize_t PosixIO::Write(int fd, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(off);
}

ssize_t PosixIO::Writev(int fd, const iovec* iov, int iovcnt) {
  // 复制 iov 数组（内核可能修改偏移），循环处理 EINTR 与部分写，语义同 Write。
  std::vector<iovec> v(iov, iov + iovcnt);
  size_t written_total = 0;
  while (!v.empty()) {
    ssize_t n = ::writev(fd, v.data(), static_cast<int>(v.size()));
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    written_total += static_cast<size_t>(n);
    size_t consumed = static_cast<size_t>(n);
    while (consumed > 0) {
      if (consumed < v[0].iov_len) {
        v[0].iov_base = static_cast<char*>(v[0].iov_base) + consumed;
        v[0].iov_len -= consumed;
        consumed = 0;
      } else {
        consumed -= v[0].iov_len;
        v.erase(v.begin());
      }
    }
  }
  return static_cast<ssize_t>(written_total);
}

ssize_t PosixIO::WritevFsync(int fd, const iovec* iov, int iovcnt) {
  ssize_t n = Writev(fd, iov, iovcnt);
  if (n < 0) return -1;
  if (Fsync(fd) != 0) return -1;
  return n;
}

ssize_t PosixIO::Pread(int fd, void* buf, size_t len, off_t off) {
  char* p = static_cast<char*>(buf);
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::pread(fd, p + got, len - got, off + static_cast<off_t>(got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) break;  // EOF
    got += static_cast<size_t>(n);
  }
  return static_cast<ssize_t>(got);
}

int PosixIO::Fsync(int fd) { return ::fsync(fd); }
int PosixIO::Ftruncate(int fd, off_t len) { return ::ftruncate(fd, len); }
int PosixIO::Close(int fd) { return ::close(fd); }

}  // namespace mycache
