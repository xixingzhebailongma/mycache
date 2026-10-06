#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "mycache/net/connection.h"
#include "mycache/protocol/resp_encoder.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/storage/db/shard.h"

namespace mycache {

class AofWriter;
struct Config;
class CommandRouter;
class PersistenceManager;

// 命令处理器执行工作并发送回复所需的全部内容。db 指向命令所属 shard 的存储；
// 无 key 的命令（PING/ECHO/INFO）在 IO 线程直接执行，db 为 nullptr。
struct CommandContext {
  std::shared_ptr<Connection> conn;
  Shard* db = nullptr;
  AofWriter* aof = nullptr;       // 非空时写命令会追加到 AOF
  const Config* config = nullptr; // 无 key 命令（如 INFO）读取全局配置
  CommandRouter* router = nullptr; // 无 key 命令聚合各 shard 统计
  PersistenceManager* pm = nullptr; // 持久化协调器（BGREWRITEAOF/SAVE/BGSAVE 用）
  uint64_t seq = 0;                 // 命令解析序号，Reply 用它保证回复有序
  std::string name{};               // 命令名（小写，诊断用）
  bool* replied = nullptr;          // 非空时 Reply 置位，供“handler 未回复”检测

  void Reply(const RespValue& v) const {
    if (replied) *replied = true;
    conn->Send(seq, name, EncodeResp(v));
  }
  void ReplySimple(const std::string& s) const { Reply(RespValue::Simple(s)); }
  void ReplyBulk(const std::string& s) const { Reply(RespValue::Bulk(s)); }
  void ReplyInt(int64_t n) const { Reply(RespValue::Integer(n)); }
  void ReplyError(const std::string& s) const { Reply(RespValue::Error(s)); }
  void ReplyNull() const { Reply(RespValue::NullBulk()); }
};

// 在可能增加内存的写命令前调用：若内存超限且无法淘汰，回 OOM 错误并返回 false。
inline bool EvictOrOom(CommandContext& ctx) {
  if (ctx.db && !ctx.db->EvictIfNeeded()) {
    ctx.ReplyError("OOM command not allowed when used memory > 'maxmemory'");
    return false;
  }
  return true;
}

}  // namespace mycache
