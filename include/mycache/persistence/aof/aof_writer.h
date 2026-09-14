#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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
class AofWriter {
 public:
  AofWriter() = default;
  ~AofWriter();

  AofWriter(const AofWriter&) = delete;
  AofWriter& operator=(const AofWriter&) = delete;

  // 打开（不存在则创建）AOF 文件并确保文件头完整。失败返回 false。
  bool Open(const std::string& path, AofFsync policy);

  // 优雅关闭：停掉后台 fsync 线程，flush + fsync + close。
  void Close();

  // 线程安全：追加一条已编码的命令记录（见 aof_format.h 的 EncodeRecord）。
  void Append(const std::string& record);

  AofFsync policy() const { return policy_; }
  bool is_open() const { return fd_ >= 0; }
  uint64_t bytes_written() const { return bytes_written_.load(); }

 private:
  bool WriteAll(const void* data, size_t len);
  void Fsync();
  void FlushLoop();  // everysec 后台线程

  int fd_ = -1;
  std::string path_;
  AofFsync policy_ = AofFsync::kEverysec;

  std::mutex mu_;        // 串行化 write / fsync
  std::condition_variable cv_;
  bool running_ = false; // 后台线程开关（mu_ 保护）
  bool dirty_ = false;   // 自上次 fsync 以来有写入（mu_ 保护）
  std::thread flush_thread_;

  std::atomic<uint64_t> bytes_written_{0};
};

// 解析配置字符串 "always"/"everysec"/"no"。非法值返回 false。
bool ParseAofFsync(const std::string& s, AofFsync* out);

}  // namespace mycache
