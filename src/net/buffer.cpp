#include "mycache/net/buffer.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace mycache {

void Buffer::ensureWritable(size_t n) {
  if (writable() >= n) return;
  if (prependable() + writable() < kCheapPrepend + n) {
    buffer_.resize(write_index_ + n);
  } else {
    size_t r = readable();
    std::memmove(begin() + kCheapPrepend, peek(), r);
    read_index_ = kCheapPrepend;
    write_index_ = kCheapPrepend + r;
  }
}

ssize_t Buffer::readFd(int fd, int* saved_errno) {
  if (writable() == 0) ensureWritable(4096);
  ssize_t n = ::read(fd, beginWrite(), writable());
  if (n < 0) {
    *saved_errno = errno;
  } else if (n > 0) {
    write_index_ += static_cast<size_t>(n);
  }
  return n;
}

ssize_t Buffer::writeFd(int fd, int* saved_errno) {
  ssize_t n = ::write(fd, peek(), readable());
  if (n > 0) {
    retrieve(static_cast<size_t>(n));
  } else if (n < 0) {
    *saved_errno = errno;
  }
  return n;
}

}  // namespace mycache
