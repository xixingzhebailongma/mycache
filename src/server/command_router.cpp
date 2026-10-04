#include "mycache/server/command_router.h"

#include "mycache/persistence/snapshot_gate.h"
#include "mycache/storage/dict/dict.h"

namespace mycache {

CommandRouter::CommandRouter(int shard_count, const EvictionConfig& eviction) {
  shards_.reserve(static_cast<size_t>(shard_count));
  for (int i = 0; i < shard_count; ++i) {
    shards_.push_back(std::make_unique<ShardWorker>(i, eviction));
  }
}

CommandRouter::~CommandRouter() { Stop(); }

void CommandRouter::Start() {
  for (auto& s : shards_) s->Start();
}

void CommandRouter::Stop() {
  for (auto& s : shards_) s->Stop();
}

size_t CommandRouter::ShardIndex(const std::string& key) const {
  return DictHash(key.data(), key.size()) % shards_.size();
}

void CommandRouter::Route(const std::string& key, std::function<void(Shard*)> fn) {
  RouteTo(ShardIndex(key), std::move(fn));
}

void CommandRouter::Route(std::string_view key, std::function<void(Shard*)> fn) {
  RouteTo(DictHash(key.data(), key.size()) % shards_.size(), std::move(fn));
}

void CommandRouter::RouteTo(size_t shard_index, std::function<void(Shard*)> fn) {
  shards_[shard_index]->Submit([fn = std::move(fn)](Shard* shard) {
    // 任务执行期间持快照闸门的 shared 锁，保证 fork 快照不会看到半修改的 shard。
    auto& gate = GlobalSnapshotGate();
    gate.LockShared();
    fn(shard);
    gate.UnlockShared();
  });
}

}  // namespace mycache
