#pragma once

#include <shared_mutex>
#include <string>
#include <vector>

#include "mycache/storage/dict/dict.h"

namespace mycache {

// 由 Redis 风格 Dict（sds 键 + Object* 值）支撑的线程安全字符串键值存储。
// 读操作加共享锁，写操作加独占锁；rehash 仅在写路径推进，因此共享锁下的读是
// 纯只读的，不会与并发写冲突。
class Keyspace {
 public:
  Keyspace();
  ~Keyspace();

  Keyspace(const Keyspace&) = delete;
  Keyspace& operator=(const Keyspace&) = delete;

  bool Get(const std::string& key, std::string* out);
  void Set(const std::string& key, const std::string& value);
  int Del(const std::vector<std::string>& keys);
  bool Exists(const std::string& key);
  size_t Size();

  // 内存 / rehash 状态（供统计与测试）。
  size_t UsedMemory() const;   // 通过跟踪分配器统计的引擎堆占用字节数
  size_t BucketCount();        // ht[0].size + ht[1].size
  bool Rehashing();

 private:
  Dict dict_;
  std::shared_mutex mu_;
};

}  // namespace mycache
