#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace mycache {

class Shard;
class CommandRouter;

// 一条多 key 命令中某个 shard 需要处理的 key。用「在原 keys 数组中的下标」表示，
// 以保留 MGET 这类需要按位置回填的语义。
struct KeyGroup {
  size_t shard_index = 0;
  std::vector<size_t> indices;  // 指向调用方传入的 keys 向量，按原顺序
};

// 把 keys 按 DictHash(key) % shard_count 分组；每个 group 内按下标升序保存。
std::vector<KeyGroup> GroupKeysByShard(const CommandRouter& router,
                                       const std::vector<std::string>& keys);

// scatter-gather：把每个 group 的 work 派发到对应 shard 线程执行，全部完成后在
// 最后一个完成的线程上调用 finish 恰好一次。work 与 finish 之间共享的聚合状态
// 由调用方用 shared_ptr 捕获持有（无锁：work 各写各的槽位/原子累加，计数用
// acq_rel 建立 happens-before）。
void FanOut(CommandRouter* router,
            const std::vector<KeyGroup>& groups,
            std::function<void(Shard*, const KeyGroup&)> work,
            std::function<void()> finish);

}  // namespace mycache
