#include "mycache/server/command_router.h"

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
  shards_[ShardIndex(key)]->Submit(std::move(fn));
}

}  // namespace mycache
