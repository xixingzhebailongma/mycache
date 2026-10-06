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
  // 多 key 命令：last_key < 0 表示 key 从 first_key 一直取到 args 末尾，>0 表示
  // 最后一个 key 的下标；0 表示单 key/无 key 命令（沿用 first_key 语义）。
  // key_step 为相邻 key 的下标差（MSET 的 key/value 交替，为 2）。
  int last_key = 0;
  int key_step = 1;
  // 回复的“完成路径”是否异步：为真表示正常路径由 FanOut 的 finish 异步回包，dispatch
  // 跳过“handler 返回未回复”的同步兜底。注意：为真时 handler 仍可能有同步早退路径
  // （如 MSET 的奇偶校验）直接 ctx.Reply 同步回；只是不能用同步兜底去判它。
  bool async_reply = false;
};

// 注册各阶段命令。
void RegisterStringCommands(CommandTable* table);
void RegisterGenericCommands(CommandTable* table);  // EXISTS/EXPIRE/TTL
void RegisterHashCommands(CommandTable* table);     // HSET/HGET/HDEL/HGETALL
void RegisterListCommands(CommandTable* table);     // LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN
void RegisterServerCommands(CommandTable* table);   // INFO/DBSIZE

}  // namespace mycache
