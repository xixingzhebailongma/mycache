#include "mycache/command/command_table.h"

#include <cstdio>

namespace mycache {

void CommandTable::Register(const Command& cmd) {
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
