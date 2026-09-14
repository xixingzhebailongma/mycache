#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "mycache/command/command.h"

namespace mycache {

// 大小写不敏感的命令注册表。
class CommandTable {
 public:
  void Register(const Command& cmd);
  const Command* Find(const std::string& name) const;

  // argc 包含命令名。失败时写入一条 Redis 错误字符串。
  static bool CheckArity(const Command& cmd, size_t argc, std::string* err);

 private:
  std::unordered_map<std::string, Command> commands_;
};

}  // namespace mycache
