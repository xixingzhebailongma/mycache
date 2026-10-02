#include "mycache/persistence/aof/aof_writer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

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
    if (WriteAll(aof::kMagic, aof::kMagicSize) != 0) {
      io_->Close(fd_);
      fd_ = -1;
      return false;
    }
  } else if (st.st_size < static_cast<off_t>(aof::kMagicSize)) {
    // 文件头在首次写入时被截断：重置为空文件再写头。
    if (io_->Ftruncate(fd_, 0) != 0 || WriteAll(aof::kMagic, aof::kMagicSize) != 0) {
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

  // 头部处理块结束后：统一 fstat(fd_) 取最终文件大小（覆盖 recovery 截断与写头两种情况），
  // 作为 current_size_ 基准。写头时 WriteAll 已递增 current_size_，这里 store 是覆盖而非累加。
  struct stat st_final {};
  if (::fstat(fd_, &st_final) == 0) {
    current_size_.store(static_cast<uint64_t>(st_final.st_size));
  } else {
    LOG_WARN("aof fstat after open failed: %s", std::strerror(errno));
    current_size_.store(0);   // 极罕见：保守置 0
  }
  base_size_.store(current_size_.load());

  if (policy_ == AofFsync::kEverysec) {
    running_ = true;
    flush_thread_ = std::thread([this] { FlushLoop(); });
  } else if (policy_ == AofFsync::kAlways) {
    group_commit_running_ = true;
    group_commit_thread_ = std::thread([this] { GroupCommitLoop(); });
  }
  last_errno_ = 0;  // reopen 复位 sticky 错误

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
    if (group_commit_running_) {
      group_commit_running_ = false;
      cv_flush_.notify_all();  // 先 notify 再 join，否则 flusher 卡在 wait
    }
  }
  if (group_commit_thread_.joinable()) group_commit_thread_.join();  // flusher 排空残余 pending_
  if (flush_thread_.joinable()) flush_thread_.join();

  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = fd_;
    fd_ = -1;                // 逻辑关闭：等待者谓词 fd_<0 成立
    batch_cv_.notify_all();  // 唤醒残留等待者
  }
  if (fd >= 0) {
    io_->Fsync(fd);  // 优雅关闭：把剩余数据落盘
    io_->Close(fd);  // 用局部 fd，非 fd_
  }
}

int AofWriter::Append(const std::string& record) {
  if (policy_ == AofFsync::kAlways) {
    uint64_t my_token;
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (fd_ < 0) return EBADF;
      if (last_errno_ != 0) return last_errno_;  // fail-stop 快路径：坏盘后直接拒
      pending_.push_back(record);
      if (rewriting_) rewrite_buffer_.append(record);  // rewrite 期间并行缓冲到 rewrite buffer
      my_token = ++submitted_;
      cv_flush_.notify_one();
    }
    std::unique_lock<std::mutex> lk(mu_);
    batch_cv_.wait(lk, [this, my_token] { return flushed_ >= my_token || fd_ < 0; });
    return (fd_ < 0) ? EBADF : last_errno_;
  }

  // everysec / no：原内联路径，仅把 write 失败带出（不再吞掉）。
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) return EBADF;
  int e = WriteAll(record.data(), record.size());
  if (e != 0) return e;
  if (rewriting_) rewrite_buffer_.append(record);  // rewrite 期间并行缓冲到 rewrite buffer
  if (policy_ == AofFsync::kEverysec) {
    dirty_ = true;
  }
  // kNo：不 fsync，交给操作系统。
  return 0;
}

int AofWriter::WriteAll(const void* data, size_t len) {
  if (io_->Write(fd_, data, len) < 0) return errno ? errno : EIO;
  bytes_written_.fetch_add(len);
  current_size_.fetch_add(len);
  return 0;
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

void AofWriter::GroupCommitLoop() {
  for (;;) {
    std::vector<std::string> batch;
    uint64_t last = 0;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_flush_.wait(lk, [this] { return !pending_.empty() || !group_commit_running_; });
      if (pending_.empty()) {
        if (!group_commit_running_) break;  // 收到 stop 且已排空
        continue;                            // 虚假唤醒
      }
      batch.swap(pending_);
      last = submitted_;
      io_in_flight_ = true;  // 标记锁外 IO：rewrite 换 fd 前需等其结束
    }
    int e = WriteBatch(batch);  // 锁外 writev+fsync；同一时刻只有一批 in-flight
    {
      std::lock_guard<std::mutex> lk(mu_);
      io_in_flight_ = false;
      if (e != 0) last_errno_ = e;  // sticky，不复位
      flushed_ = last;              // 成功或失败都推进（已处理水位）
      batch_cv_.notify_all();
      io_done_cv_.notify_all();
    }
  }
}

int AofWriter::WriteBatch(const std::vector<std::string>& batch) {
  std::vector<iovec> iov;
  iov.reserve(batch.size());
  size_t total = 0;
  for (const auto& r : batch) {
    iov.push_back({const_cast<char*>(r.data()), r.size()});
    total += r.size();
  }
  if (io_->WritevFsync(fd_, iov.data(), static_cast<int>(iov.size())) < 0) return errno ? errno : EIO;
  bytes_written_.fetch_add(total);
  current_size_.fetch_add(total);
  return 0;
}

void AofWriter::BeginRewrite() {
  std::unique_lock<std::mutex> lock(mu_);
  io_done_cv_.wait(lock, [this] { return !io_in_flight_; });  // fork 前不留 in-flight AOF IO
  rewriting_ = true;
  rewrite_buffer_.clear();
}

bool AofWriter::CompleteRewrite(const std::string& temp_path) {
  std::unique_lock<std::mutex> lock(mu_);
  io_done_cv_.wait(lock, [this] { return !io_in_flight_; });  // 换 fd 前等锁外 IO 结束
  if (fd_ < 0) {                       // AOF 已关闭（不应发生）：temp 仍可能是子进程产物，一并清理
    ::unlink(temp_path.c_str());
    return false;
  }

  // 开 temp（O_APPEND 保证补写/后续追加都在文件尾）。此 fd 之后直接成为新 fd_。
  // 注意：以下所有 return false 都发生在 rename 提交点之前，旧 fd_ 完好，安全；且都
  // 【不】清 rewriting_/base_size_（统一由 OnChildExit 调 AbortRewrite 清理），但都会
  // ::unlink(temp) 自清理，不依赖上层兜底。
  int tmp = io_->Open(temp_path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC, 0644);
  if (tmp < 0) {
    LOG_ERROR("aof rewrite: open temp failed: %s", std::strerror(errno));
    ::unlink(temp_path.c_str());
    return false;
  }
  if (!rewrite_buffer_.empty() &&
      io_->Write(tmp, rewrite_buffer_.data(), rewrite_buffer_.size()) < 0) {
    io_->Close(tmp);
    ::unlink(temp_path.c_str());
    return false;
  }
  if (io_->Fsync(tmp) != 0) {
    LOG_ERROR("aof rewrite: fsync temp failed: %s", std::strerror(errno));
    io_->Close(tmp);
    ::unlink(temp_path.c_str());
    return false;
  }

  if (::rename(temp_path.c_str(), path_.c_str()) != 0) {
    LOG_ERROR("aof rewrite: rename failed: %s", std::strerror(errno));
    io_->Close(tmp);
    ::unlink(temp_path.c_str());
    return false;
  }

  // 提交点已过：tmp 的 inode 已被 rename 到 path_（temp_path 名已消失，无需 unlink）。
  // 关旧 fd，tmp 就地成为新 fd（无需按名 reopen）。
  if (io_->Close(fd_) != 0) {
    LOG_DEBUG("aof rewrite: close old fd failed: %s", std::strerror(errno));
  }
  fd_ = tmp;

  struct stat st {};
  if (::fstat(fd_, &st) == 0) {
    current_size_.store(static_cast<uint64_t>(st.st_size));   // reset 成紧凑文件大小
    base_size_.store(current_size_.load());  // 增长基准 = rewrite 后大小（Redis 语义：记住最新 rewrite 后的 AOF 大小）
  } else {
    LOG_WARN("aof rewrite: fstat after swap failed: %s", std::strerror(errno));
    current_size_.store(0);   // 保守置 0，后续 Append 重新累计
  }

  rewrite_buffer_.clear();
  rewriting_ = false;
  last_errno_ = 0;  // 新文件已 fsync，复位 sticky 错误
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
