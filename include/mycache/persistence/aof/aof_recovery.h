#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mycache {

class Shard;

struct AofRecoveryResult {
  bool ok = false;
  uint64_t commands_replayed = 0;  // 成功重放的命令数
  uint64_t bytes_truncated = 0;    // 修复尾部不完整记录时丢弃的字节数
  std::string error;               // 失败原因（ok == false 时有效）
};

// AOF 恢复：读取文件、校验每条记录的 checksum、逐条重放，并修复 kill -9 造成的
// 尾部不完整记录（partial write）。中间的记录损坏则视为致命错误。
class AofRecovery {
 public:
  // apply(name, args) 把一条已解析（命令名已小写）的命令应用到存储，返回 false
  // 表示该命令无法重放（视为致命错误）。不存在的文件视为空 AOF（ok）。
  static AofRecoveryResult Replay(
      const std::string& path,
      const std::function<bool(const std::string& name,
                               const std::vector<std::string>& args)>& apply);
};

// 把一条 AOF 命令应用到单个 shard（无需路由，按 args[0] 定位 key）。
// 支持 SET / DEL / HSET / HDEL / PEXPIREAT；返回 false 表示未知或不支持的命令。
bool ApplyAofCommand(Shard* shard, const std::string& name,
                     const std::vector<std::string>& args);

}  // namespace mycache
