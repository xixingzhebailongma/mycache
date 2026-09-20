#include "mycache/persistence/aof/aof_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "mycache/common/logger.h"
#include "mycache/persistence/aof/aof_format.h"

namespace mycache {

namespace {
constexpr auto kFlushInterval = std::chrono::milliseconds(1000);
}

AofWriter::AofWriter(IOEngine* io) : io_(io) {}

AofWriter::~AofWriter() { Close(); }

bool AofWriter::Open(const std::string& path, AofFsync policy) {
  path_ = path;
  policy_ = policy;

  if (!io_) {  // 未注入引擎时（单测等）回退到 posix。
    owned_io_ = CreateIOEngine("posix");
    io_ = owned_io_.get();
  }

  fd_ = io_->Open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    LOG_ERROR("aof open failed: %s (%s)", path.c_str(), std::strerror(errno));
    return false;
  }

  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    LOG_ERROR("aof fstat failed: %s", std::strerror(errno));
    io_->Close(fd_);
    fd_ = -1;
    return false;
  }

  if (st.st_size == 0) {
    // 新文件：写入文件头。
    if (!WriteAll(aof::kMagic, aof::kMagicSize)) {
      io_->Close(fd_);
      fd_ = -1;
      return false;
    }
  } else if (st.st_size < static_cast<off_t>(aof::kMagicSize)) {
    // 文件头在首次写入时被截断：重置为空文件再写头。
    if (io_->Ftruncate(fd_, 0) != 0 || !WriteAll(aof::kMagic, aof::kMagicSize)) {
      io_->Close(fd_);
      fd_ = -1;
      return false;
    }
  } else {
    // 已有文件：校验文件头是否完整。
    char magic[aof::kMagicSize];
    ssize_t n = io_->Pread(fd_, magic, aof::kMagicSize, 0);
    if (n != static_cast<ssize_t>(aof::kMagicSize) ||
        std::memcmp(magic, aof::kMagic, aof::kMagicSize) != 0) {
      LOG_ERROR("aof file has unexpected header: %s", path.c_str());
      io_->Close(fd_);
      fd_ = -1;
      return false;
    }
  }

  if (policy_ == AofFsync::kEverysec) {
    running_ = true;
    flush_thread_ = std::thread([this] { FlushLoop(); });
  }

  LOG_INFO("aof opened: %s (appendfsync %s, io %s)", path.c_str(),
           policy_ == AofFsync::kAlways ? "always"
           : policy_ == AofFsync::kEverysec ? "everysec"
                                            : "no",
           io_->Name());
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
    io_->Fsync(fd_);  // 优雅关闭：把剩余数据落盘
    io_->Close(fd_);
    fd_ = -1;
  }
}

void AofWriter::Append(const std::string& record) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) return;
  if (!WriteAll(record.data(), record.size())) return;
  if (rewriting_) rewrite_buffer_.append(record);  // rewrite 期间并行缓冲到 rewrite buffer

  if (policy_ == AofFsync::kAlways) {
    Fsync();
  } else if (policy_ == AofFsync::kEverysec) {
    dirty_ = true;
  }
  // kNo：不 fsync，交给操作系统。
}

bool AofWriter::WriteAll(const void* data, size_t len) {
  if (io_->Write(fd_, data, len) < 0) return false;
  bytes_written_.fetch_add(len);
  return true;
}

void AofWriter::Fsync() {
  if (fd_ < 0) return;
  if (io_->Fsync(fd_) != 0) {
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
      if (fd_ >= 0) io_->Fsync(fd_);  // 持锁 fsync，避免与写路径交错
    }
  }
}

void AofWriter::BeginRewrite() {
  std::lock_guard<std::mutex> lock(mu_);
  rewriting_ = true;
  rewrite_buffer_.clear();
}

bool AofWriter::CompleteRewrite(const std::string& temp_path) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) {
    rewriting_ = false;
    rewrite_buffer_.clear();
    return false;
  }

  // 1) 打开子进程写好的 temp 文件（追加），把 rewrite 期间的并发写入补进去并 fsync。
  int tmp = io_->Open(temp_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (tmp < 0) {
    LOG_ERROR("aof rewrite: open temp failed: %s", std::strerror(errno));
    return false;
  }
  if (!rewrite_buffer_.empty() &&
      io_->Write(tmp, rewrite_buffer_.data(), rewrite_buffer_.size()) < 0) {
    io_->Close(tmp);
    return false;
  }
  if (io_->Fsync(tmp) != 0) {
    LOG_ERROR("aof rewrite: fsync temp failed: %s", std::strerror(errno));
    io_->Close(tmp);
    return false;
  }
  io_->Close(tmp);

  // 2) 原子替换旧日志，并让 fd_ 指向新文件。全程持 mu_，Append 不会插进来。
  if (::rename(temp_path.c_str(), path_.c_str()) != 0) {
    LOG_ERROR("aof rewrite: rename failed: %s", std::strerror(errno));
    return false;
  }
  io_->Close(fd_);
  fd_ = io_->Open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    LOG_ERROR("aof rewrite: reopen failed: %s", std::strerror(errno));
  }

  rewrite_buffer_.clear();
  rewriting_ = false;
  return true;
}

void AofWriter::AbortRewrite() {
  std::lock_guard<std::mutex> lock(mu_);
  rewrite_buffer_.clear();
  rewriting_ = false;
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
