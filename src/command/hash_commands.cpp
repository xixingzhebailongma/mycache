#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/protocol/resp_value.h"

namespace mycache {

namespace {

// key 不存在或为 hash 时返回 true；否则回复 WRONGTYPE 并返回 false。
bool CheckHashType(CommandContext& ctx, const std::string& key) {
  std::string t = ctx.db->Type(key);
  if (t == "hash" || t == "none") return true;
  ctx.ReplyError("WRONGTYPE Operation against a key holding the wrong kind of value");
  return false;
}

void HSetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckHashType(ctx, args[1].str)) return;
  if (!EvictOrOom(ctx)) return;
  int added = ctx.db->HSet(args[1].str, args[2].str, args[3].str);
  aof::LogCommand(ctx.aof, "HSET", {args[1].str, args[2].str, args[3].str});
  ctx.ReplyInt(added);
}

void HGetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckHashType(ctx, args[1].str)) return;
  std::string v;
  if (ctx.db->HGet(args[1].str, args[2].str, &v)) ctx.ReplyBulk(v);
  else ctx.ReplyNull();
}

void HDelCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckHashType(ctx, args[1].str)) return;
  std::vector<std::string> fields;
  fields.reserve(args.size() - 2);
  for (size_t i = 2; i < args.size(); ++i) fields.push_back(args[i].str);
  int removed = ctx.db->HDel(args[1].str, fields);
  if (removed > 0) {
    std::vector<std::string> log = {args[1].str};
    log.insert(log.end(), fields.begin(), fields.end());
    aof::LogCommand(ctx.aof, "HDEL", log);
  }
  ctx.ReplyInt(removed);
}

void HGetAllCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  if (!CheckHashType(ctx, args[1].str)) return;
  auto fields = ctx.db->HGetAll(args[1].str);
  std::vector<RespValue> arr;
  arr.reserve(fields.size() * 2);
  for (auto& kv : fields) {
    arr.push_back(RespValue::Bulk(kv.first));
    arr.push_back(RespValue::Bulk(kv.second));
  }
  ctx.Reply(RespValue::Array(std::move(arr)));
}

}  // namespace

void RegisterHashCommands(CommandTable* table) {
  table->Register(Command{"hset", 4, 1, HSetCommand});
  table->Register(Command{"hget", 3, 1, HGetCommand});
  table->Register(Command{"hdel", -3, 1, HDelCommand});
  table->Register(Command{"hgetall", 2, 1, HGetAllCommand});
}

}  // namespace mycache
