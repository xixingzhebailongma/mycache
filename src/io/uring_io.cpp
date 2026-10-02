#include "mycache/io/uring_io.h"

#include <cerrno>
#include <vector>

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

ssize_t UringIO::WritevFsync(int fd, const iovec* iov, int iovcnt) {
  if (!ok_) return posix_.WritevFsync(fd, iov, iovcnt);
  // 复制 iov（内核可能改写偏移），循环处理部分写：每轮 linked(writev -> fsync)。
  std::vector<iovec> v(iov, iov + iovcnt);
  size_t written_total = 0;
  while (!v.empty()) {
    io_uring_sqe* wq = io_uring_get_sqe(&ring_);
    if (!wq) return -1;
    io_uring_prep_writev(wq, fd, v.data(), static_cast<unsigned>(v.size()),
                         static_cast<__u64>(-1));
    wq->flags |= IOSQE_IO_LINK;  // 本轮 writev 完成后才执行 fsync
    io_uring_sqe* fq = io_uring_get_sqe(&ring_);
    if (!fq) return -1;
    io_uring_prep_fsync(fq, fd, 0);  // 0 = 完整 fsync
    if (io_uring_submit(&ring_) < 0) return -1;

    // 等两个 CQE：先 writev（res = 写入字节数或 -errno），后 fsync。
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(&ring_, &cqe) < 0) return -1;
    int wres = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    if (io_uring_wait_cqe(&ring_, &cqe) < 0) return -1;
    int fres = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);

    if (wres < 0) { errno = -wres; return -1; }
    if (fres < 0) { errno = -fres; return -1; }
    written_total += static_cast<size_t>(wres);

    // 按已写字节推进 iov slice，剩余字节由下一轮 WRITEV+FSYNC 补齐（短写时）。
    size_t consumed = static_cast<size_t>(wres);
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

}  // namespace mycache

#endif  // MYCACHE_HAVE_IO_URING
