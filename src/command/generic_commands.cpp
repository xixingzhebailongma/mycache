#include <cstdint>
#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
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
  int count = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    if (ctx.db->Exists(args[i].str)) {
      ++count;
      Metrics::Instance().IncrHits();
    } else {
      Metrics::Instance().IncrMisses();
    }
  }
  ctx.ReplyInt(count);
}

void ExpireCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  long long seconds = 0;
  if (!ParseIntArg(ctx, args[2], &seconds)) return;
  int64_t when = UnixMs() + static_cast<int64_t>(seconds) * 1000;
  if (ctx.db->ExpireAt(args[1].str, when)) {
    // 记绝对时间 PEXPIREAT，重放时不受启动时机影响。
    aof::LogCommand(ctx.aof, "PEXPIREAT", {args[1].str, std::to_string(when)});
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
    aof::LogCommand(ctx.aof, "PEXPIREAT", {args[1].str, std::to_string(when)});
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
  table->Register(Command{"exists", -2, 1, ExistsCommand});
  table->Register(Command{"expire", 3, 1, ExpireCommand});
  table->Register(Command{"pexpire", 3, 1, PExpireCommand});
  table->Register(Command{"ttl", 2, 1, TtlCommand});
}

}  // namespace mycache
