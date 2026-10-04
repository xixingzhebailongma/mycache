#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/command/multi_key.h"
#include "mycache/common/time.h"
#include "mycache/monitor/metrics.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

namespace {

// 把参数解析为整数；失败时回复错误并返回 false。
bool ParseIntArg(CommandContext& ctx, const RespValue& arg, long long* out) {
  if (!StringToLongLong(arg.str.data(), arg.str.size(), out)) {
    ctx.ReplyError("ERR value is not an integer or out of range");
    return false;
  }
  return true;
}

void ExistsCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  auto keys = std::make_shared<std::vector<std::string>>();
  keys->reserve(args.size() - 1);
  for (size_t i = 1; i < args.size(); ++i) keys->push_back(args[i].str);

  auto groups = GroupKeysByShard(*ctx.router, *keys);
  auto total = std::make_shared<std::atomic<int64_t>>(0);
  std::shared_ptr<Connection> conn = ctx.conn;

  FanOut(ctx.router, groups,
         [keys, total](Shard* shard, const KeyGroup& g) {
           int n = 0;
           // 重复 key 按 Redis 语义每个出现都计数（同一 key 必落在同一 shard，
           // 逐 key 判断即可保留重复语义）。
           for (size_t i : g.indices) {
             if (shard->Exists((*keys)[i])) {
               ++n;
               Metrics::Instance().IncrHits();
             } else {
               Metrics::Instance().IncrMisses();
             }
           }
           total->fetch_add(n, std::memory_order_relaxed);
         },
         [conn, total]() {
           conn->Send(EncodeResp(
               RespValue::Integer(total->load(std::memory_order_relaxed))));
         });
}

void ExpireCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  long long seconds = 0;
  if (!ParseIntArg(ctx, args[2], &seconds)) return;
  int64_t when = UnixMs() + static_cast<int64_t>(seconds) * 1000;
  if (ctx.db->ExpireAt(args[1].str, when)) {
    // 记绝对时间 PEXPIREAT，重放时不受启动时机影响。
    if (aof::LogCommand(ctx.aof, "PEXPIREAT", args[1].str, std::to_string(when)) != 0) {
      ctx.ReplyError("AOF write failed");
      return;
    }
    ctx.ReplyInt(1);
  } else {
    ctx.ReplyInt(0);
  }
}

void PExpireCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  long long ms = 0;
  if (!ParseIntArg(ctx, args[2], &ms)) return;
  int64_t when = UnixMs() + static_cast<int64_t>(ms);
  if (ctx.db->ExpireAt(args[1].str, when)) {
    if (aof::LogCommand(ctx.aof, "PEXPIREAT", args[1].str, std::to_string(when)) != 0) {
      ctx.ReplyError("AOF write failed");
      return;
    }
    ctx.ReplyInt(1);
  } else {
    ctx.ReplyInt(0);
  }
}

void TtlCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  int64_t ms = ctx.db->TtlMs(args[1].str);
  // Redis 的 TTL 返回秒，且向最近秒舍入。
  ctx.ReplyInt(ms < 0 ? ms : (ms + 500) / 1000);
}

}  // namespace

void RegisterGenericCommands(CommandTable* table) {
  table->Register(Command{"exists", -2, 1, ExistsCommand, -1, 1});
  table->Register(Command{"expire", 3, 1, ExpireCommand});
  table->Register(Command{"pexpire", 3, 1, PExpireCommand});
  table->Register(Command{"ttl", 2, 1, TtlCommand});
}

}  // namespace mycache
