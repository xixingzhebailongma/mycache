#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/monitor/metrics.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

namespace {

void PingCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (args.size() > 2) {
    ctx.ReplyError("ERR wrong number of arguments for 'ping' command");
    return;
  }
  if (args.size() == 2) {
    ctx.ReplyBulk(args[1].str);
  } else {
    ctx.ReplySimple("PONG");
  }
}

void SetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!EvictOrOom(ctx)) return;
  ctx.db->Set(args[1].str, args[2].str);
  aof::LogCommand(ctx.aof, "SET", {args[1].str, args[2].str});
  ctx.ReplySimple("OK");
}

void GetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  std::string value;
  if (ctx.db->Get(args[1].str, &value)) {
    Metrics::Instance().IncrHits();
    ctx.ReplyBulk(value);
  } else {
    Metrics::Instance().IncrMisses();
    ctx.ReplyNull();
  }
}

void DelCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  std::vector<std::string> keys;
  keys.reserve(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) keys.push_back(args[i].str);
  int removed = ctx.db->Del(keys);
  if (removed > 0) aof::LogCommand(ctx.aof, "DEL", keys);
  ctx.ReplyInt(removed);
}

// ECHO 消息 -> 批量字符串回复。redis-cli --pipe 用它作为同步哨兵，
// 以检测所有流水线回复是否都已收到。
void EchoCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  ctx.ReplyBulk(args[1].str);
}

// ---- INCR / DECR ----

void IncrByCommand(CommandContext& ctx, const std::vector<RespValue>& args, long long delta) {
  if (!EvictOrOom(ctx)) return;

  std::string cur;
  long long v = 0;
  if (ctx.db->Get(args[1].str, &cur)) {
    if (!StringToLongLong(cur.data(), cur.size(), &v)) {
      ctx.ReplyError("ERR value is not an integer or out of range");
      return;
    }
  }

  long long result = 0;
  if (__builtin_add_overflow(v, delta, &result)) {
    ctx.ReplyError("ERR increment or decrement would overflow");
    return;
  }

  // 记成 SET（绝对结果），AOF 重放时结果确定，且不依赖 INCR 的增量语义。
  std::string nv = std::to_string(result);
  ctx.db->Set(args[1].str, nv);
  aof::LogCommand(ctx.aof, "SET", {args[1].str, nv});
  ctx.ReplyInt(result);
}

void IncrCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  IncrByCommand(ctx, args, 1);
}

void DecrCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  IncrByCommand(ctx, args, -1);
}

void IncrByExplicitCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  long long by = 0;
  if (!StringToLongLong(args[2].str.data(), args[2].str.size(), &by)) {
    ctx.ReplyError("ERR value is not an integer or out of range");
    return;
  }
  IncrByCommand(ctx, args, by);
}

void DecrByExplicitCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  long long by = 0;
  if (!StringToLongLong(args[2].str.data(), args[2].str.size(), &by)) {
    ctx.ReplyError("ERR value is not an integer or out of range");
    return;
  }
  IncrByCommand(ctx, args, -by);
}

}  // namespace

void RegisterStringCommands(CommandTable* table) {
  table->Register(Command{"ping", -1, 0, PingCommand});
  table->Register(Command{"set", 3, 1, SetCommand});
  table->Register(Command{"get", 2, 1, GetCommand});
  table->Register(Command{"del", -2, 1, DelCommand});
  table->Register(Command{"echo", 2, 0, EchoCommand});
  table->Register(Command{"incr", 2, 1, IncrCommand});
  table->Register(Command{"decr", 2, 1, DecrCommand});
  table->Register(Command{"incrby", 3, 1, IncrByExplicitCommand});
  table->Register(Command{"decrby", 3, 1, DecrByExplicitCommand});
}

}  // namespace mycache
