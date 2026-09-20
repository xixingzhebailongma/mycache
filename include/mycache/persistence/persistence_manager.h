#pragma once

#include <memory>
#include <string>

#include <sys/types.h>  // pid_t

namespace mycache {

class AofWriter;
class CommandRouter;
class IOEngine;
struct Config;

// 持久化协调器：持有 AofWriter，管理 AOF 的恢复/打开/关闭，以及后台 rewrite 子进程
// 的 fork、SIGCHLD 收割与收尾。由 Server 持有；命令层通过 CommandContext::pm 触发
// BGREWRITEAOF（后续周次的 SAVE/BGSAVE 也走这里）。
class PersistenceManager {
 public:
  PersistenceManager() = default;
  ~PersistenceManager();

  PersistenceManager(const PersistenceManager&) = delete;
  PersistenceManager& operator=(const PersistenceManager&) = delete;

  // 启动阶段调用：若 appendonly，则恢复 + 打开 AOF。失败返回 false（拒绝启动）。
  bool Init(const Config& config, CommandRouter* router);
  // 优雅关闭：flush + fsync + close AOF。可重复调用。
  void Close();

  AofWriter* aof() const { return aof_.get(); }
  bool aof_enabled() const { return aof_enabled_; }

  // ---- 后台子进程 ----
  bool HasChild() const { return child_pid_ > 0; }

  // BGREWRITEAOF：fork 子进程重写 AOF。已在重写/有子进程/AOF 未开时填 err 返回 false。
  bool StartRewrite(std::string* err);
  // SAVE：同步快照到 RDB（阻塞；持快照闸门 exclusive 锁遍历各 shard）。
  bool Save(std::string* err);
  // BGSAVE：fork 子进程异步快照到 RDB。
  bool BgSave(std::string* err);
  // Server 的 SIGCHLD 处理里调用：收割子进程并完成 rewrite/bgsave 收尾。
  void OnChildExit(pid_t pid, int status);

 private:
  bool ReplayAndOpen(const Config& config, CommandRouter* router);
  bool LoadRdbIfPresent(const Config& config, CommandRouter* router);
  void FinishBgSave(bool ok);

  std::unique_ptr<AofWriter> aof_;
  std::unique_ptr<IOEngine> io_;
  bool aof_enabled_ = false;
  std::string aof_path_;
  std::string rdb_path_;

  CommandRouter* router_ = nullptr;

  pid_t child_pid_ = -1;
  int child_kind_ = 0;  // 0=无 1=rewrite 2=bgsave
  std::string child_tmp_path_;  // 子进程写快照的临时文件
};

}  // namespace mycache
