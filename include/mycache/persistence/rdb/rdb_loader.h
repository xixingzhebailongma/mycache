#pragma once

#include <cstdint>
#include <string>

namespace mycache {

class CommandRouter;

struct RdbLoadResult {
  bool ok = false;
  uint64_t keys_loaded = 0;
  std::string error;
};

// 把 RDB 文件加载进 router 各 shard。启动阶段、shard worker 启动之前单线程调用，
// 因此可直接写入 shard 而无需加锁；可自由分配。
RdbLoadResult RdbLoad(const std::string& path, CommandRouter* router);

}  // namespace mycache
