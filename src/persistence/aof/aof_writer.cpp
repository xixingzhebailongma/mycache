#include "mycache/persistence/aof/aof_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "mycache/common/logger.h"
#include "mycache/persistence/aof/aof_format.h"

namespace mycache {

namespace {
constexpr auto kFlushInterval = std::chrono::milliseconds(1000);
}

AofWriter::~AofWriter() { Close(); }

bool AofWriter::Open(const std::string& path, AofFsync policy) {
  path_ = path;
  policy_ = policy;

  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    LOG_ERROR("aof open failed: %s (%s)", path.c_str(), std::strerror(errno));
    return false;
  }

  struct stat st{};
  if (::fstat(fd_, &st) != 0) {
    LOG_ERROR("aof fstat failed: %s", std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  if (st.st_size == 0) {
    // 新文件：写入文件头。
    if (!WriteAll(aof::kMagic, aof::kMagicSize)) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
  } else if (st.st_size < static_cast<off_t>(aof::kMagicSize)) {
    // 文件头在首次写入时被截断：重置为空文件再写头。
    if (::ftruncate(fd_, 0) != 0 || !WriteAll(aof::kMagic, aof::kMagicSize)) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
  } else {
    // 已有文件：校验文件头是否完整。
    char magic[aof::kMagicSize];
    ssize_t n = ::pread(fd_, magic, aof::kMagicSize, 0);
    if (n != static_cast<ssize_t>(aof::kMagicSize) ||
        std::memcmp(magic, aof::kMagic, aof::kMagicSize) != 0) {
      LOG_ERROR("aof file has unexpected header: %s", path.c_str());
      ::close(fd_);
      fd_ = -1;
      return false;
    }
  }

  if (policy_ == AofFsync::kEverysec) {
    running_ = true;
    flush_thread_ = std::thread([this] { FlushLoop(); });
  }

  LOG_INFO("aof opened: %s (appendfsync %s)", path.c_str(),
           policy_ == AofFsync::kAlways ? "always"
           : policy_ == AofFsync::kEverysec ? "everysec"
                                            : "no");
  return true;
}

void AofWriter::Close() {
  if (fd_ < 0) return;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) {
      running_ = false;
      cv_.notify_all();
    }
  }
  if (flush_thread_.joinable()) flush_thread_.join();

  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ >= 0) {
    ::fsync(fd_);  // 优雅关闭：把剩余数据落盘
    ::close(fd_);
    fd_ = -1;
  }
}

void AofWriter::Append(const std::string& record) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) return;
  if (!WriteAll(record.data(), record.size())) return;

  if (policy_ == AofFsync::kAlways) {
    Fsync();
  } else if (policy_ == AofFsync::kEverysec) {
    dirty_ = true;
  }
  // kNo：不 fsync，交给操作系统。
}

bool AofWriter::WriteAll(const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  size_t written = 0;
  while (written < len) {
    ssize_t n = ::write(fd_, p + written, len - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG_ERROR("aof write failed: %s", std::strerror(errno));
      return false;
    }
    written += static_cast<size_t>(n);
  }
  bytes_written_.fetch_add(len);
  return true;
}

void AofWriter::Fsync() {
  if (fd_ < 0) return;
  if (::fsync(fd_) != 0) {
    LOG_ERROR("aof fsync failed: %s", std::strerror(errno));
  }
}

void AofWriter::FlushLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (running_) {
    bool stop = cv_.wait_for(lock, kFlushInterval, [this] { return !running_; });
    if (stop) break;
    if (dirty_) {
      dirty_ = false;
      if (fd_ >= 0) ::fsync(fd_);  // 持锁 fsync，避免与写路径交错
    }
  }
}

bool ParseAofFsync(const std::string& s, AofFsync* out) {
  if (s == "always") {
    *out = AofFsync::kAlways;
    return true;
  }
  if (s == "everysec") {
    *out = AofFsync::kEverysec;
    return true;
  }
  if (s == "no") {
    *out = AofFsync::kNo;
    return true;
  }
  return false;
}

}  // namespace mycache
