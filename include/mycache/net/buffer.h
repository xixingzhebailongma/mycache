#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace mycache {

// 一个带前置空间的连续字节缓冲区，同时用于连接上的读（输入）与写（输出）。
class Buffer {
 public:
  static const size_t kCheapPrepend = 8;
  static const size_t kInitialSize = 4096;

  Buffer()
      : buffer_(kCheapPrepend + kInitialSize),
        read_index_(kCheapPrepend),
        write_index_(kCheapPrepend) {}

  size_t readable() const { return write_index_ - read_index_; }
  size_t writable() const { return buffer_.size() - write_index_; }
  size_t prependable() const { return read_index_; }

  const char* peek() const { return begin() + read_index_; }
  char* beginWrite() { return begin() + write_index_; }

  void retrieve(size_t n) {
    if (n < readable()) {
      read_index_ += n;
    } else {
      retrieveAll();
    }
  }

  void retrieveAll() { read_index_ = write_index_ = kCheapPrepend; }

  std::string retrieveAsString(size_t n) {
    std::string s(peek(), n);
    retrieve(n);
    return s;
  }

  std::string readAllAsString() { return retrieveAsString(readable()); }

  void append(const char* data, size_t n) {
    ensureWritable(n);
    std::copy(data, data + n, beginWrite());
    write_index_ += n;
  }

  void append(const std::string& s) { append(s.data(), s.size()); }

  // 执行一次读取；返回读取的字节数，遇到 EOF 返回 0，出错返回 -1（errno 已保存）。
  ssize_t readFd(int fd, int* saved_errno);

  // 执行一次写入；返回写入的字节数，出错返回 -1（errno 已保存）。
  ssize_t writeFd(int fd, int* saved_errno);

 private:
  void ensureWritable(size_t n);

  char* begin() { return buffer_.data(); }
  const char* begin() const { return buffer_.data(); }

  std::vector<char> buffer_;
  size_t read_index_;
  size_t write_index_;
};

}  // namespace mycache
