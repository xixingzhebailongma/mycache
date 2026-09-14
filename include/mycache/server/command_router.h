#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mycache/eviction/eviction_policy.h"
#include "mycache/server/shard_worker.h"

namespace mycache {

class Shard;

// 根据 key 的哈希把命令任务路由到对应的 shard worker。shard 数量在启动时确定。
class CommandRouter {
 public:
  CommandRouter(int shard_count, const EvictionConfig& eviction);
  ~CommandRouter();

  void Start();
  void Stop();

  size_t shard_count() const { return shards_.size(); }
  size_t ShardIndex(const std::string& key) const;

  // 直接访问第 i 个 shard 的存储（仅供启动阶段的 AOF 恢复等单线程场景使用）。
  Shard* ShardAt(size_t i) const { return shards_[i]->shard(); }

  // 把任务提交到 key 对应的 shard worker（在其线程上执行）。
  void Route(const std::string& key, std::function<void(Shard*)> fn);

 private:
  std::vector<std::unique_ptr<ShardWorker>> shards_;
};

}  // namespace mycache
