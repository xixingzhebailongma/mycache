#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/command/multi_key.h"
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
  auto keys = std::make_shared<std::vector<std::string>>();
  keys->reserve(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) keys->push_back(args[i].str);

  auto groups = GroupKeysByShard(*ctx.router, *keys);
  auto total = std::make_shared<std::atomic<int64_t>>(0);
  std::shared_ptr<Connection> conn = ctx.conn;
  AofWriter* aof = ctx.aof;

  FanOut(ctx.router, groups,
         [keys, total, aof](Shard* shard, const KeyGroup& g) {
           std::vector<std::string> sub;
           sub.reserve(g.indices.size());
           for (size_t i : g.indices) sub.push_back((*keys)[i]);
           int removed = shard->Del(sub);
           // 逐 shard 记日志：每条 DEL 的 key 都落在同一个 shard，AOF 恢复按
           // 首 key 路由不会漏删跨 shard 的 key。
           if (removed > 0) aof::LogCommand(aof, "DEL", sub);
           total->fetch_add(removed, std::memory_order_relaxed);
         },
         [conn, total]() {
           conn->Send(EncodeResp(
               RespValue::Integer(total->load(std::memory_order_relaxed))));
         });
}

void MGetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  auto keys = std::make_shared<std::vector<std::string>>();
  keys->reserve(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) keys->push_back(args[i].str);

  size_t n = keys->size();
  auto results = std::make_shared<std::vector<RespValue>>(n);  // 默认 kNull
  auto groups = GroupKeysByShard(*ctx.router, *keys);
  std::shared_ptr<Connection> conn = ctx.conn;

  FanOut(ctx.router, groups,
         [keys, results](Shard* shard, const KeyGroup& g) {
           for (size_t i : g.indices) {
             std::string v;
             if (shard->Get((*keys)[i], &v)) {
               (*results)[i] = RespValue::Bulk(v);
               Metrics::Instance().IncrHits();
             } else {
               (*results)[i] = RespValue::NullBulk();
               Metrics::Instance().IncrMisses();
             }
           }
         },
         [conn, results]() {
           conn->Send(EncodeResp(RespValue::Array(*results)));
         });
}

void MSetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  // args: MSET k1 v1 k2 v2 ...（arity -3 保证至少 3 个参数）
  if (args.size() % 2 == 0) {
    ctx.ReplyError("ERR wrong number of arguments for 'mset' command");
    return;
  }
  auto keys = std::make_shared<std::vector<std::string>>();
  auto values = std::make_shared<std::vector<std::string>>();
  keys->reserve((args.size() - 1) / 2);
  values->reserve((args.size() - 1) / 2);
  for (size_t i = 1; i < args.size(); i += 2) {
    keys->push_back(args[i].str);
    values->push_back(args[i + 1].str);
  }

  auto groups = GroupKeysByShard(*ctx.router, *keys);
  auto oom = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<Connection> conn = ctx.conn;
  AofWriter* aof = ctx.aof;

  FanOut(ctx.router, groups,
         [keys, values, oom, aof](Shard* shard, const KeyGroup& g) {
           // 每个 shard 写自己的 key 前检查内存（跨 shard 不保证原子，见方案说明）。
           if (!shard->EvictIfNeeded()) {
             oom->store(true, std::memory_order_relaxed);
             return;
           }
           for (size_t i : g.indices) {
             shard->Set((*keys)[i], (*values)[i]);
             // 记成逐条 SET：幂等自包含，恢复按首 key 路由正确。
             aof::LogCommand(aof, "SET", {(*keys)[i], (*values)[i]});
           }
         },
         [conn, oom]() {
           if (oom->load(std::memory_order_relaxed)) {
             conn->Send(EncodeResp(RespValue::Error(
                 "OOM command not allowed when used memory > 'maxmemory'")));
           } else {
             conn->Send(EncodeResp(RespValue::Simple("OK")));
           }
         });
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
  table->Register(Command{"del", -2, 1, DelCommand, -1, 1});
  table->Register(Command{"mget", -2, 1, MGetCommand, -1, 1});
  table->Register(Command{"mset", -3, 1, MSetCommand, -1, 2});
  table->Register(Command{"echo", 2, 0, EchoCommand});
  table->Register(Command{"incr", 2, 1, IncrCommand});
  table->Register(Command{"decr", 2, 1, DecrCommand});
  table->Register(Command{"incrby", 3, 1, IncrByExplicitCommand});
  table->Register(Command{"decrby", 3, 1, DecrByExplicitCommand});
}

}  // namespace mycache
