#include "mycache/io/posix_io.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

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
