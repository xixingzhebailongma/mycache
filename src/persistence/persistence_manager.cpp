#include "mycache/persistence/persistence_manager.h"

#include <sys/wait.h>
#include <unistd.h>

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

bool PersistenceManager::StartRewrite(std::string* err) {
  if (!aof_enabled_ || !aof_) {
    *err = "ERR AOF is not enabled";
    return false;
  }
  if (child_pid_ > 0) {
    *err = "ERR background save/rewrite already in progress";
    return false;
  }
  if (aof_->rewriting()) {
    *err = "ERR Background append only file rewriting already in progress";
    return false;
  }

  // 子进程缓冲（fork 前分配，避免子进程 malloc 死锁）。
  std::vector<char> out_buf(kRewriteOutCap);
  std::vector<char> rec_buf(kRewriteRecCap);
  child_tmp_path_ = aof_path_ + ".rewrite.tmp";

  // 快照闸门：排空所有在途 shard 任务后 fork，保证 COW 快照一致；随后 BeginRewrite
  // 让 fork 之后的写入进入 rewrite buffer，二者衔接无缝隙。
  auto& gate = GlobalSnapshotGate();
  gate.LockExclusive();
  aof_->BeginRewrite();

  pid_t pid = ::fork();
  if (pid == 0) {
    // 子进程：只做无分配操作，永不返回。
    RewriteAofChild(router_, child_tmp_path_.c_str(),
                    out_buf.data(), out_buf.size(), rec_buf.data(), rec_buf.size());
    // RewriteAofChild 内部 _exit，不会走到这里。
  }

  gate.UnlockExclusive();

  if (pid < 0) {
    aof_->AbortRewrite();
    *err = "ERR fork failed";
    return false;
  }

  child_pid_ = pid;
  child_kind_ = 1;  // rewrite
  LOG_INFO("AOF rewrite started (child pid %d)", static_cast<int>(pid));
  return true;
}

bool PersistenceManager::Save(std::string* err) {
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

bool PersistenceManager::BgSave(std::string* err) {
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
