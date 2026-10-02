#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/monitor/info.h"
#include "mycache/persistence/persistence_manager.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/server/command_router.h"

namespace mycache {

namespace {

void InfoCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.config || !ctx.router) {
    ctx.ReplyError("ERR internal: server context unavailable");
    return;
  }
  ctx.ReplyBulk(BuildInfo(*ctx.config, *ctx.router));
}

void DbSizeCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.router) {
    ctx.ReplyError("ERR internal: server context unavailable");
    return;
  }
  size_t n = 0;
  for (size_t i = 0; i < ctx.router->shard_count(); ++i) {
    n += ctx.router->ShardAt(i)->Size();
  }
  ctx.ReplyInt(static_cast<int64_t>(n));
}

void BgRewriteAofCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.pm) {
    ctx.ReplyError("ERR internal: persistence manager unavailable");
    return;
  }
  std::string err;
  if (ctx.pm->StartRewrite(&err)) {
    ctx.ReplySimple("Background append only file rewriting started");
  } else {
    ctx.ReplyError(err);
  }
}

void SaveCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.pm) {
    ctx.ReplyError("ERR internal: persistence manager unavailable");
    return;
  }
  std::string err;
  if (ctx.pm->Save(&err)) ctx.ReplySimple("OK");
  else ctx.ReplyError(err);
}

void BgSaveCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.pm) {
    ctx.ReplyError("ERR internal: persistence manager unavailable");
    return;
  }
  std::string err;
  if (ctx.pm->BgSave(&err)) ctx.ReplySimple("Background saving started");
  else ctx.ReplyError(err);
}

// LOAD：运行时从 RDB 整库重载，覆盖当前内存数据（DEBUG RELOAD 风格）。
// 当前 arity=1、不支持 LOAD <filename>；未来若支持指定文件，需改 arity 为 -1
// 并在 handler 里解析 args[1] 作为路径。
void LoadCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  (void)args;
  if (!ctx.pm) {
    ctx.ReplyError("ERR internal: persistence manager unavailable");
    return;
  }
  std::string err;
  if (ctx.pm->Load(&err)) ctx.ReplySimple("OK");
  else ctx.ReplyError(err);
}

}  // namespace

void RegisterServerCommands(CommandTable* table) {
  table->Register(Command{"info", -1, 0, InfoCommand});
  table->Register(Command{"dbsize", 1, 0, DbSizeCommand});
  table->Register(Command{"bgrewriteaof", 1, 0, BgRewriteAofCommand});
  table->Register(Command{"save", 1, 0, SaveCommand});
  table->Register(Command{"bgsave", 1, 0, BgSaveCommand});
  table->Register(Command{"load", 1, 0, LoadCommand});
}

}  // namespace mycache
