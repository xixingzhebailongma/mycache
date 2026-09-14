#include <string>
#include <vector>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/command/command_table.h"
#include "mycache/monitor/info.h"
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

}  // namespace

void RegisterServerCommands(CommandTable* table) {
  table->Register(Command{"info", -1, 0, InfoCommand});
  table->Register(Command{"dbsize", 1, 0, DbSizeCommand});
}

}  // namespace mycache
