#pragma once

#include <cstdint>
#include <string>

namespace mycache {

struct Config;
class CommandRouter;

// 组装 INFO 命令的输出文本（server/clients/memory/stats/keyspace 各节）。
// 连接数与统计计数从 Metrics 单例读取。
std::string BuildInfo(const Config& config, const CommandRouter& router);

}  // namespace mycache
