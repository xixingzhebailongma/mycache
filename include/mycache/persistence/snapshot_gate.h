#pragma once

#include <mutex>
#include <shared_mutex>

namespace mycache {

// 快照闸门（reader-writer 锁）：所有会读写 shard 存储的任务（命令任务 + 各 worker
// 的定时器兜底淘汰/过期/rehash）在执行期间持 shared 锁；fork 做快照（AOF rewrite /
// BGSAVE）之前持 exclusive 锁。
//
// 为什么需要它：fork() 只把调用它的那一条线程带进子进程，且子进程看到的是 fork
// 那一瞬间的内存快照。若某个 shard worker 此刻正把 dict 改到一半（rehash 搬迁、
// 桶链指针半更新），子进程就会读到"撕碎的中间态"。exclusive 锁保证 fork 时没有任何
// shard 任务在途，快照因此一致。shared 锁之间互不排斥，所以正常读写仍完全并发，
// 只有极低频的 fork 才会短暂排他。
class SnapshotGate {
 public:
  void LockShared() { mu_.lock_shared(); }
  void UnlockShared() { mu_.unlock_shared(); }
  void LockExclusive() { mu_.lock(); }
  void UnlockExclusive() { mu_.unlock(); }

  // RAII 守卫（异常安全）：返回持独占锁的 unique_lock，作用域结束（含异常栈展开）自动解锁。
  // Load() 这类长临界区用它替代裸 Lock/Unlock，避免 FlushAll/RdbLoad 抛异常时漏解锁死锁。
  std::unique_lock<std::shared_mutex> ExclusiveLock() {
    return std::unique_lock<std::shared_mutex>(mu_);
  }

 private:
  std::shared_mutex mu_;
};

// 进程级单例（懒初始化；只在父进程使用，子进程从不触碰）。
inline SnapshotGate& GlobalSnapshotGate() {
  static SnapshotGate gate;
  return gate;
}

}  // namespace mycache
