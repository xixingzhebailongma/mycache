#include "mycache/io/uring_io.h"

#ifdef MYCACHE_HAVE_IO_URING

namespace mycache {

UringIO::UringIO() {
  if (io_uring_queue_init(256, &ring_, 0) == 0) ok_ = true;
}

UringIO::~UringIO() {
  if (ok_) io_uring_queue_exit(&ring_);
}

ssize_t UringIO::Write(int fd, const void* buf, size_t len) {
  if (!ok_) return posix_.Write(fd, buf, len);
  const char* p = static_cast<const char*>(buf);
  size_t off = 0;
  while (off < len) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return -1;
    // offset = -1：使用文件当前位置（O_APPEND 语义由内核 write 路径保证）。
    io_uring_prep_write(sqe, fd, p + off, len - off, static_cast<__u64>(-1));
    if (io_uring_submit(&ring_) < 0) return -1;
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(&ring_, &cqe) < 0) return -1;
    int res = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    if (res < 0) return -1;
    off += static_cast<size_t>(res);
  }
  return static_cast<ssize_t>(off);
}

int UringIO::Fsync(int fd) {
  if (!ok_) return posix_.Fsync(fd);
  io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
  if (!sqe) return -1;
  io_uring_prep_fsync(sqe, fd, 0);  // 0 = 完整 fsync
  if (io_uring_submit(&ring_) < 0) return -1;
  io_uring_cqe* cqe = nullptr;
  if (io_uring_wait_cqe(&ring_, &cqe) < 0) return -1;
  int res = cqe->res;
  io_uring_cqe_seen(&ring_, cqe);
  return res < 0 ? -1 : 0;
}

}  // namespace mycache

#endif  // MYCACHE_HAVE_IO_URING
