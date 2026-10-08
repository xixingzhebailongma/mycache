#include "mycache/persistence/persistence_manager.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "mycache/common/config.h"
#include "mycache/common/logger.h"
#include "mycache/io/io_engine.h"
#include "mycache/persistence/aof/aof_recovery.h"
#include "mycache/persistence/aof/aof_rewrite.h"
#include "mycache/persistence/aof/aof_writer.h"
#include "mycache/persistence/rdb/rdb_loader.h"
#include "mycache/persistence/rdb/rdb_writer.h"
#include "mycache/persistence/snapshot_gate.h"
#include "mycache/server/command_router.h"

namespace {

// 系统未装 <linux/dirent.h>（缺 linux-libc-dev），手动定义 getdents64 返回项。
// 该布局是 UAPI ABI 稳定结构，d_name 最多 NAME_MAX+1（255+1）。
struct LinuxDirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
};

// 子进程 fork 后立即调用：关闭除 0/1/2 外全部继承 fd，避免 rewrite/bgsave 子进程
// 持有父进程的监听 socket（父进程被 kill -9 后端口仍被孤儿子进程占用，重启 EADDRINUSE）。
// 全程不 malloc：fork 时另一线程可能持 malloc arena 锁，只能用 syscall + 栈 buffer。
void CloseInheritedFds() {
  // 1) close_range（glibc ≥2.34 已声明，需 _GNU_SOURCE；Linux ≥5.9）。
  if (::close_range(3, ~0U, 0) == 0) return;
  if (errno != ENOSYS && errno != EINVAL) return;  // 真错误，放弃回退

  // 2) 回退：getdents64 读 /proc/self/fd 把 fd 号收进栈数组，再统一关（不边遍历边关）。
  int dir = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) return;
  int fds[1024];
  int n = 0;
  char buf[4096];
  for (;;) {
    long len = ::syscall(SYS_getdents64, dir, buf, sizeof buf);
    if (len <= 0) break;
    for (long off = 0; off < len;) {
      auto* d = reinterpret_cast<LinuxDirent64*>(buf + off);
      int fd = std::atoi(d->d_name);
      if (fd > 2 && fd != dir) {
        if (n == 1024) {  // 满则 flush：先关掉已收这批再继续（处理 >1024 fd）
          for (int i = 0; i < n; ++i) ::close(fds[i]);
          n = 0;
        }
        fds[n++] = fd;
      }
      off += d->d_reclen;
    }
  }
  ::close(dir);
  for (int i = 0; i < n; ++i) ::close(fds[i]);
}

}  // namespace

namespace mycache {

PersistenceManager::~PersistenceManager() { Close(); }

bool PersistenceManager::ReplayAndOpen(const Config& config, CommandRouter* router) {
  AofFsync fsync = AofFsync::kEverysec;
  if (!ParseAofFsync(config.appendfsync, &fsync)) {
    LOG_ERROR("invalid appendfsync value: %s", config.appendfsync.c_str());
    return false;
  }
  aof_path_ = config.AofPath();

  // 1) 恢复：逐条重放并修复尾部不完整记录（shard worker 尚未启动，可安全直接写）。
  auto result = AofRecovery::Replay(
      aof_path_, [router](const std::string& name, const std::vector<std::string>& args) {
        if (args.empty()) return false;
        size_t idx = router->ShardIndex(args[0]);
        return ApplyAofCommand(router->ShardAt(idx), name, args);
      });
  if (!result.ok) {
    LOG_ERROR("AOF recovery failed: %s", result.error.c_str());
    return false;
  }
  LOG_INFO("AOF recovery: replayed %llu command(s), truncated %llu trailing byte(s)",
           static_cast<unsigned long long>(result.commands_replayed),
           static_cast<unsigned long long>(result.bytes_truncated));

  // 2) 打开追加写入。
  aof_ = std::make_unique<AofWriter>(io_.get());
  if (!aof_->Open(aof_path_, fsync)) {
    aof_.reset();
    return false;
  }
  return true;
}

bool PersistenceManager::LoadRdbIfPresent(const Config& config, CommandRouter* router) {
  (void)config;
  if (::access(rdb_path_.c_str(), F_OK) != 0) return true;  // 无 RDB，空库启动
  RdbLoadResult r = RdbLoad(rdb_path_, router);
  if (!r.ok) {
    LOG_ERROR("RDB load failed: %s", r.error.c_str());
    return false;
  }
  LOG_INFO("RDB loaded: %llu key(s)", static_cast<unsigned long long>(r.keys_loaded));
  return true;
}

bool PersistenceManager::Init(const Config& config, CommandRouter* router) {
  router_ = router;
  rdb_path_ = config.RdbPath();
  // ParseMemorySize 已拒绝负值；这里再防御性 clamp：负值按 0 处理，避免 static_cast 把
  // 负数 wrap 成超大 uint64_t，令 ShouldAutoRewrite 的 current<=min_size 恒真、永不触发。
  aof_rewrite_min_size_ = config.aof_rewrite_min_size > 0
                              ? static_cast<uint64_t>(config.aof_rewrite_min_size)
                              : 0;
  aof_rewrite_percentage_ = config.aof_rewrite_percentage;
  io_ = CreateIOEngine(config.io_engine);
  if (config.appendonly) {
    aof_enabled_ = true;
    return ReplayAndOpen(config, router);
  }
  return LoadRdbIfPresent(config, router);
}

void PersistenceManager::Close() {
  if (aof_) aof_->Close();
  aof_.reset();
}

bool PersistenceManager::MaybeAutoRewrite() {
  if (!aof_ || !aof_enabled_) return false;   // 无 AOF：静默跳过
  if (HasChild()) return false;               // 已有子进程（rewrite/bgsave 进行中）
  if (aof_->rewriting()) return false;        // 已在 rewrite

  uint64_t current = aof_->current_size();
  uint64_t base = aof_->base_size();
  if (!ShouldAutoRewrite(current, base, aof_rewrite_min_size_, aof_rewrite_percentage_)) {
    return false;
  }

  std::string err;
  if (StartRewrite(&err)) {
    LOG_INFO("auto AOF rewrite triggered (current %llu, base %llu)",
             static_cast<unsigned long long>(current),
             static_cast<unsigned long long>(base));
    return true;
  }
  LOG_WARN("auto AOF rewrite failed to start: %s", err.c_str());
  return false;
}

bool PersistenceManager::StartRewrite(std::string* err) {
  if (!aof_enabled_ || !aof_) {
    *err = "ERR AOF is not enabled";
    return false;
  }

  // 子进程缓冲（锁外、fork 前预分配，避免子进程 malloc 死锁；分配失败无需回滚）。
  std::vector<char> out_buf(kRewriteOutCap);
  std::vector<char> rec_buf(kRewriteRecCap);

  std::lock_guard<std::mutex> lock(child_mu_);   // 检查 + fork 同一临界区，消除 TOCTOU
  if (child_pid_ > 0) {
    *err = "ERR background save/rewrite already in progress";
    return false;
  }
  if (aof_->rewriting()) {
    *err = "ERR Background append only file rewriting already in progress";
    return false;
  }

  child_tmp_path_ = aof_path_ + ".rewrite.tmp";

  // 快照闸门：排空所有在途 shard 任务后 fork，保证 COW 快照一致；随后 BeginRewrite
  // 让 fork 之后的写入进入 rewrite buffer，二者衔接无缝隙。
  auto& gate = GlobalSnapshotGate();
  gate.LockExclusive();
  aof_->BeginRewrite();

  pid_t pid = ::fork();
  if (pid == 0) {
    CloseInheritedFds();  // 先关继承 fd（监听 socket 等），再进 rewrite
    // 子进程：只做无分配操作，永不返回。
    RewriteAofChild(router_, child_tmp_path_.c_str(),
                    out_buf.data(), out_buf.size(), rec_buf.data(), rec_buf.size());
    // RewriteAofChild 内部 _exit，不会走到这里。
  }

  gate.UnlockExclusive();

  if (pid < 0) {
    aof_->AbortRewrite();   // 回滚 rewriting_ 与 base_size_
    *err = "ERR fork failed";
    return false;
  }

  child_pid_ = pid;
  child_kind_ = 1;  // rewrite
  LOG_INFO("AOF rewrite started (child pid %d)", static_cast<int>(pid));
  return true;
}

bool PersistenceManager::Save(std::string* err) {
  std::lock_guard<std::mutex> lock(child_mu_);
  if (child_pid_ > 0) {
    *err = "ERR background save/rewrite already in progress";
    return false;
  }

  std::vector<char> out_buf(kRdbOutCap);
  std::vector<char> rec_buf(kRdbRecCap);
  std::string tmp = rdb_path_ + ".tmp";

  // 持快照闸门 exclusive 锁：排空所有在途 shard 任务后，在调用线程单线程遍历写快照
  // （此时无 worker 改 shard，安全且可自由分配）。SAVE 因此是阻塞的。
  auto& gate = GlobalSnapshotGate();
  gate.LockExclusive();
  bool ok = RdbWriteSnapshot(router_, tmp.c_str(), out_buf.data(), out_buf.size(),
                             rec_buf.data(), rec_buf.size());
  gate.UnlockExclusive();

  if (!ok) {
    ::unlink(tmp.c_str());
    *err = "ERR RDB save failed";
    return false;
  }
  if (::rename(tmp.c_str(), rdb_path_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    *err = "ERR RDB rename failed";
    return false;
  }
  return true;
}

bool PersistenceManager::Load(std::string* err) {
  if (aof_enabled_) {
    *err = "ERR LOAD is not supported when appendonly is enabled (AOF would desync)";
    return false;
  }
  if (::access(rdb_path_.c_str(), F_OK) != 0) {
    *err = "ERR no RDB file to load (run SAVE/BGSAVE first)";
    return false;
  }

  RdbLoadResult r;
  {
    // RAII 独占锁：排空在途命令 + 定时器任务；即使 FlushAll/RdbLoad 抛异常
    // （如 std::bad_alloc），unique_lock 析构也会自动解锁，不会死锁。
    // RdbLoad 必须在独占锁内、单线程调用：其内部用 router->ShardAt() 直接写 shard，
    // 不访问 worker 队列，也不依赖任何 worker 线程路径。
    auto gate_lock = GlobalSnapshotGate().ExclusiveLock();
    for (size_t i = 0; i < router_->shard_count(); ++i) {
      router_->ShardAt(i)->FlushAll();
    }
    r = RdbLoad(rdb_path_, router_);
  }  // 离开作用域，gate_lock 析构自动 UnlockExclusive

  if (!r.ok) {
    *err = "ERR LOAD failed: " + r.error + " (database was flushed)";
    return false;
  }
  LOG_INFO("LOAD: reloaded %llu key(s) from %s",
           static_cast<unsigned long long>(r.keys_loaded), rdb_path_.c_str());
  return true;
}

bool PersistenceManager::BgSave(std::string* err) {
  std::lock_guard<std::mutex> lock(child_mu_);
  if (child_pid_ > 0) {
    *err = "ERR background save/rewrite already in progress";
    return false;
  }

  std::vector<char> out_buf(kRdbOutCap);
  std::vector<char> rec_buf(kRdbRecCap);
  child_tmp_path_ = rdb_path_ + ".tmp";

  auto& gate = GlobalSnapshotGate();
  gate.LockExclusive();

  pid_t pid = ::fork();
  if (pid == 0) {
    CloseInheritedFds();  // 先关继承 fd（监听 socket 等），再写 RDB
    WriteRdbChild(router_, child_tmp_path_.c_str(),
                  out_buf.data(), out_buf.size(), rec_buf.data(), rec_buf.size());
    // WriteRdbChild 内部 _exit，不会走到这里。
  }

  gate.UnlockExclusive();

  if (pid < 0) {
    *err = "ERR fork failed";
    return false;
  }

  child_pid_ = pid;
  child_kind_ = 2;  // bgsave
  LOG_INFO("RDB background save started (child pid %d)", static_cast<int>(pid));
  return true;
}

void PersistenceManager::FinishBgSave(bool ok) {
  if (ok && ::rename(child_tmp_path_.c_str(), rdb_path_.c_str()) == 0) {
    LOG_INFO("RDB background save completed");
  } else {
    LOG_WARN("RDB background save failed, keeping old snapshot");
    ::unlink(child_tmp_path_.c_str());
  }
}

void PersistenceManager::OnChildExit(pid_t pid, int status) {
  std::lock_guard<std::mutex> lock(child_mu_);
  if (pid != child_pid_) return;
  int kind = child_kind_;
  child_pid_ = -1;
  child_kind_ = 0;

  bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (kind == 1) {  // rewrite
    if (exited_ok) {
      if (aof_ && aof_->CompleteRewrite(child_tmp_path_)) {
        LOG_INFO("AOF rewrite completed");
      } else {
        LOG_ERROR("AOF rewrite completion failed, keeping old log");
        if (aof_) aof_->AbortRewrite();
        ::unlink(child_tmp_path_.c_str());
      }
    } else {
      LOG_WARN("AOF rewrite child failed (status %d), keeping old log", status);
      if (aof_) aof_->AbortRewrite();
      ::unlink(child_tmp_path_.c_str());
    }
  } else if (kind == 2) {  // bgsave
    FinishBgSave(exited_ok);
  }
}

}  // namespace mycache
