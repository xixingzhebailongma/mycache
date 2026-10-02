#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <sys/types.h>  // pid_t

namespace mycache {

class AofWriter;
class CommandRouter;
class IOEngine;
struct Config;

// 纯函数：是否应触发自动 rewrite（percentage<=0 = 关闭；__int128 防乘法溢出）。
inline bool ShouldAutoRewrite(uint64_t current, uint64_t base,
                              uint64_t min_size, int percentage) {
  if (percentage <= 0) return false;          // 关闭（含负值）
  if (current <= min_size) return false;
  if (base == 0) base = 1;
  if (current <= base) return false;          // 未增长
  unsigned __int128 grew = static_cast<unsigned __int128>(current - base) * 100;
  unsigned __int128 thr  = static_cast<unsigned __int128>(base) * percentage;
  return grew >= thr;  // 等价 Redis 的 (current*100/base - 100) >= percentage
}

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
  bool HasChild() const { std::lock_guard<std::mutex> lk(child_mu_); return child_pid_ > 0; }

  // BGREWRITEAOF：fork 子进程重写 AOF。已在重写/有子进程/AOF 未开时填 err 返回 false。
  bool StartRewrite(std::string* err);
  // 自动 rewrite 检查（Server 主循环 timerfd tick 调用）：AOF 增长超阈值时触发 StartRewrite。
  bool MaybeAutoRewrite();
  // SAVE：同步快照到 RDB（阻塞；持快照闸门 exclusive 锁遍历各 shard）。
  bool Save(std::string* err);
  // LOAD：运行时从 RDB 整库重载（DEBUG RELOAD 风格）：清空所有 shard 后加载 rdb_path_。
  // 阻塞式；在 GlobalSnapshotGate 独占锁内执行，安全。appendonly 开启时拒绝。
  // 失败后 DB 已被清空（不保证跨 shard 原子，与 Redis DEBUG RELOAD 一致）。
  bool Load(std::string* err);
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

  mutable std::mutex child_mu_;  // 串行化 StartRewrite/Save/BgSave 检查+fork 与 OnChildExit 状态更新
  uint64_t aof_rewrite_min_size_ = 64ull << 20;
  int aof_rewrite_percentage_ = 100;
};

}  // namespace mycache
