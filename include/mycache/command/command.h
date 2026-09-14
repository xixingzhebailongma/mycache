#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mycache {

struct CommandContext;
struct RespValue;
class CommandTable;

// 一条已注册的命令。`arity` 遵循 Redis 语义：正值表示参数个数必须恰好等于该值
// （含命令名本身）；负值表示"至少 |arity| 个参数"。
struct Command {
  std::string name;
  int arity;
  // 路由：first_key 是路由 key 在 args（含命令名）中的下标；0 表示无 key，
  // 该命令会在调用线程（IO 线程）直接执行而不路由到 shard。
  int first_key = 0;
  std::function<void(CommandContext&, const std::vector<RespValue>&)> handler;
};

// 注册各阶段命令。
void RegisterStringCommands(CommandTable* table);
void RegisterGenericCommands(CommandTable* table);  // EXISTS/EXPIRE/TTL
void RegisterHashCommands(CommandTable* table);     // HSET/HGET/HDEL/HGETALL
void RegisterListCommands(CommandTable* table);     // LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN
void RegisterServerCommands(CommandTable* table);   // INFO/DBSIZE

}  // namespace mycache
