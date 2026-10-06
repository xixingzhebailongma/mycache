#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mycache/io/io_engine.h"

namespace mycache {

// AOF 落盘策略，对应 Redis 的 appendfsync：
//   kAlways   每条记录 write + fsync（最安全、最慢）
//   kEverysec 每条记录 write，后台线程每秒 fsync 一次（默认）
//   kNo       每条记录 write，不 fsync（交给操作系统，最快）
enum class AofFsync { kAlways, kEverysec, kNo };

// AOF 写入器：把已编码的命令记录追加到磁盘文件。
//
// 所有 shard worker 线程共享同一个 AofWriter，并发追加由内部互斥锁串行化，
// 因此多条记录不会交错。三种策略都保证每条记录在返回前已经 write() 到内核页缓存
// （进程被 kill -9 后仍存活）；差异只在于 fsync 的时机，即对断电/宕机的持久性。
//
// 顺序不变量（与网络层耦合，改动前必读）：
//  1. 客户端回复序由 Connection 的 reorder buffer 强制成命令解析序（connection.cpp
//     SendInLoop），与本文件的追加序相互独立。
//  2. AOF 追加序是 worker 完成序：跨 shard 命令谁先拿到 mu_ 谁先写，不对齐解析序/回复序。
//     跨 shard 顺序对重放无影响（重放按 key 路由到各自 shard）。
//  3. ack-after-fsync 由「单条命令内 LogCommand（write+fsync）先于 ctx.Reply」保证，
//     不依赖任何跨命令顺序；reorder buffer 只延迟回复、不提前回复。
//  4. 引入 MULTI/EXEC 时，第 2 条的「AOF 追加序 ≠ 回复序」解耦失效：MULTI 要求跨命令
//     原子性与顺序，追加序必须与命令序对齐。但第 3 条的 ack-after-fsync 不受影响——
//     它只依赖单命令内 LogCommand 先于 Reply。
class AofWriter {
 public:
  // io 为空时在 Open 里回退为内部持有的 PosixIO（方便单测直接构造）。
  explicit AofWriter(IOEngine* io = nullptr);
  ~AofWriter();

  AofWriter(const AofWriter&) = delete;
  AofWriter& operator=(const AofWriter&) = delete;

  // 打开（不存在则创建）AOF 文件并确保文件头完整。失败返回 false。
  bool Open(const std::string& path, AofFsync policy);

  // 优雅关闭：停掉后台 fsync 线程，flush + fsync + close。
  void Close();

  // 线程安全：追加一条已编码的命令记录（见 aof_format.h 的 EncodeRecord）。
  // 返回 0 成功，非 0 = errno。always 档在此阻塞等待本批 fsync 完成（组提交）。
  int Append(const std::string& record);

  AofFsync policy() const { return policy_; }
  bool is_open() const { return fd_ >= 0; }
  uint64_t bytes_written() const { return bytes_written_.load(); }
  uint64_t current_size() const { return current_size_.load(); }
  uint64_t base_size() const { return base_size_.load(); }

  // ---- rewrite（Week 6）----
  // 开始缓冲并发追加：置 rewriting_=true 并清空 rewrite_buffer_。须在 fork 前、持
  // 快照闸门 exclusive 锁时调用，保证 buffer 只捕获 fork 之后的写入。
  void BeginRewrite();
  // 完成 rewrite：在 mu_ 下把 rewrite_buffer_ 追加到 temp_path 打开的文件并 fsync，
  // 再 rename(temp_path, path_) 换掉旧日志，close 旧 fd 并重开新文件，最后清 buffer。
  // 失败返回 false 且不做 rename（旧日志保持完好）。
  bool CompleteRewrite(const std::string& temp_path);
  // 放弃 rewrite：清 buffer、rewriting_=false（子进程失败时调用，绝不 rename）。
  void AbortRewrite();
  bool rewriting() const { return rewriting_.load(); }

 private:
  int WriteAll(const void* data, size_t len);          // 返回 0 / errno
  void FlushLoop();                                     // everysec 后台线程
  void GroupCommitLoop();                               // always 档组提交 flusher
  int WriteBatch(const std::vector<std::string>& batch); // writev+fsync，返回 0 / errno

  int fd_ = -1;
  std::string path_;
  AofFsync policy_ = AofFsync::kEverysec;
  IOEngine* io_ = nullptr;                    // 注入的引擎（由 PersistenceManager 持有）
  std::unique_ptr<IOEngine> owned_io_;        // io_ 为空时的 posix 兜底

  std::mutex mu_;        // 串行化 write / fsync / rewrite 换 fd
  std::condition_variable cv_;
  bool running_ = false; // 后台线程开关（mu_ 保护）
  bool dirty_ = false;   // 自上次 fsync 以来有写入（mu_ 保护）
  std::thread flush_thread_;

  // ---- group commit（always）----
  std::vector<std::string> pending_;       // 待 flush 记录（mu_ 保护）
  std::condition_variable cv_flush_;       // 唤醒 flusher（mu_ 保护）
  std::condition_variable batch_cv_;       // 唤醒等待者（mu_ 保护）
  std::condition_variable io_done_cv_;     // 本轮锁外 IO 已结束（mu_ 保护）
  bool group_commit_running_ = false;      // 组提交 flusher 开关（mu_ 保护）
  std::thread group_commit_thread_;
  bool io_in_flight_ = false;              // flusher 是否正在锁外 writev+fsync（mu_ 保护）
  uint64_t submitted_ = 0;                 // 已 append 记录总数（token 来源，mu_ 保护）
  uint64_t flushed_ = 0;                   // 已处理水位：成功或失败都推进（mu_ 保护）
  int last_errno_ = 0;                     // sticky 失败 errno（mu_ 保护）

  std::atomic<bool> rewriting_{false};   // 是否正在 AOF rewrite（原子，rewriting() 无锁读安全）
  std::string rewrite_buffer_;           // rewrite 期间并发写入的原始记录字节（mu_ 保护）

  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> current_size_{0};       // 当前 AOF 文件大小（Append/CompleteRewrite 维护）
  std::atomic<uint64_t> base_size_{0};          // 最新一次 rewrite 完成后的文件大小，作为增长基准（CompleteRewrite 更新）
};

// 解析配置字符串 "always"/"everysec"/"no"。非法值返回 false。
bool ParseAofFsync(const std::string& s, AofFsync* out);

}  // namespace mycache
