#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

namespace {

// key 不存在或为 list 时返回 true；否则回复 WRONGTYPE 并返回 false。
bool CheckListType(CommandContext& ctx, const std::string& key) {
  std::string t = ctx.db->Type(key);
  if (t == "list" || t == "none") return true;
  ctx.ReplyError("WRONGTYPE Operation against a key holding the wrong kind of value");
  return false;
}

// 收集 LPUSH/RPUSH 的多个 value 参数（下标 2 起）。
std::vector<std::string> CollectValues(const std::vector<RespValue>& args) {
  std::vector<std::string> values;
  values.reserve(args.size() - 2);
  for (size_t i = 2; i < args.size(); ++i) values.push_back(args[i].str);
  return values;
}

void LPushCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  if (!EvictOrOom(ctx)) return;
  auto values = CollectValues(args);
  int len = ctx.db->LPush(args[1].str, values);
  // 记成 LPUSH（参数与收到的命令一致），重放时按同样顺序重建列表。
  std::vector<std::string> log = {args[1].str};
  log.insert(log.end(), values.begin(), values.end());
  aof::LogCommand(ctx.aof, "LPUSH", log);
  ctx.ReplyInt(len);
}

void RPushCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  if (!EvictOrOom(ctx)) return;
  auto values = CollectValues(args);
  int len = ctx.db->RPush(args[1].str, values);
  std::vector<std::string> log = {args[1].str};
  log.insert(log.end(), values.begin(), values.end());
  aof::LogCommand(ctx.aof, "RPUSH", log);
  ctx.ReplyInt(len);
}

void LPopCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  std::string v;
  if (ctx.db->LPop(args[1].str, &v)) {
    // 弹出是破坏性写，记成 LPOP（重放时逐次弹出，效果一致）。
    aof::LogCommand(ctx.aof, "LPOP", {args[1].str});
    ctx.ReplyBulk(v);
  } else {
    ctx.ReplyNull();
  }
}

void RPopCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  std::string v;
  if (ctx.db->RPop(args[1].str, &v)) {
    aof::LogCommand(ctx.aof, "RPOP", {args[1].str});
    ctx.ReplyBulk(v);
  } else {
    ctx.ReplyNull();
  }
}

void LRangeCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  long long start = 0, stop = 0;
  if (!StringToLongLong(args[2].str.data(), args[2].str.size(), &start) ||
      !StringToLongLong(args[3].str.data(), args[3].str.size(), &stop)) {
    ctx.ReplyError("ERR value is not an integer or out of range");
    return;
  }
  auto vals = ctx.db->LRange(args[1].str, static_cast<long>(start), static_cast<long>(stop));
  std::vector<RespValue> arr;
  arr.reserve(vals.size());
  for (auto& v : vals) arr.push_back(RespValue::Bulk(std::move(v)));
  ctx.Reply(RespValue::Array(std::move(arr)));
}

void LLenCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckListType(ctx, args[1].str)) return;
  ctx.ReplyInt(static_cast<int64_t>(ctx.db->LLen(args[1].str)));
}

}  // namespace

void RegisterListCommands(CommandTable* table) {
  table->Register(Command{"lpush", -3, 1, LPushCommand});
  table->Register(Command{"rpush", -3, 1, RPushCommand});
  table->Register(Command{"lpop", 2, 1, LPopCommand});
  table->Register(Command{"rpop", 2, 1, RPopCommand});
  table->Register(Command{"lrange", 4, 1, LRangeCommand});
  table->Register(Command{"llen", 2, 1, LLenCommand});
}

}  // namespace mycache
