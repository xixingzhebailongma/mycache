#include "mycache/command/multi_key.h"

#include <memory>

#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"

namespace mycache {

std::vector<KeyGroup> GroupKeysByShard(const CommandRouter& router,
                                       const std::vector<std::string>& keys) {
  std::vector<KeyGroup> groups;
  for (size_t i = 0; i < keys.size(); ++i) {
    size_t s = router.ShardIndex(keys[i]);
    bool found = false;
    for (auto& g : groups) {
      if (g.shard_index == s) {
        g.indices.push_back(i);
        found = true;
        break;
      }
    }
    if (!found) groups.push_back(KeyGroup{s, {i}});
  }
  return groups;
}

namespace {

// 扇入计数状态：每个 shard 子任务完成后 Complete()，最后一个完成者触发 on_last。
struct FanOutState {
  std::atomic<size_t> remaining;
  std::function<void()> on_last;
  FanOutState(size_t n, std::function<void()> f) : remaining(n), on_last(std::move(f)) {}

  void Complete() {
    // acq_rel：release 侧发布本任务写入的聚合结果，最后一个的 acquire 侧读到
    // 所有先前的 release，从而 on_last 能看到全部部分结果。
    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) on_last();
  }
};

}  // namespace

void FanOut(CommandRouter* router,
            const std::vector<KeyGroup>& groups,
            std::function<void(Shard*, const KeyGroup&)> work,
            std::function<void()> finish) {
  if (groups.empty()) {
    finish();  // 防御：arity 保证多 key 命令至少 1 个 key，正常不会走到
    return;
  }
  auto state = std::make_shared<FanOutState>(groups.size(), std::move(finish));
  for (const auto& g : groups) {
    router->RouteTo(g.shard_index, [state, work, g](Shard* shard) {
      work(shard, g);
      state->Complete();
    });
  }
}

}  // namespace mycache
