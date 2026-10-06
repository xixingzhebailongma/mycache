#include "mycache/command/command_table.h"

#include <cstdio>
#include <cstdlib>

#include "mycache/common/logger.h"

namespace mycache {

void CommandTable::Register(const Command& cmd) {
  // 启动自检：async_reply 命令必须 last_key != 0（走多 key 分支、finish 异步回包）。
  // 漏填 last_key 会把它落到同步分支，被“未回复”兜底误判。程序员错误，fail fast。
  if (cmd.async_reply && cmd.last_key == 0) {
    LOG_FATAL("command '%s': async_reply requires last_key != 0", cmd.name.c_str());
    std::exit(1);  // exit 走 atexit/stdio flush，日志能看到；abort 会 SIGABRT+core
  }
  commands_[cmd.name] = cmd;
}

const Command* CommandTable::Find(const std::string& name) const {
  auto it = commands_.find(name);
  return it == commands_.end() ? nullptr : &it->second;
}

bool CommandTable::CheckArity(const Command& cmd, size_t argc, std::string* err) {
  bool ok;
  if (cmd.arity >= 0) {
    ok = (argc == static_cast<size_t>(cmd.arity));
  } else {
    ok = (argc >= static_cast<size_t>(-cmd.arity));
  }
  if (!ok) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "ERR wrong number of arguments for '%s' command",
                  cmd.name.c_str());
    *err = buf;
  }
  return ok;
}

}  // namespace mycache
