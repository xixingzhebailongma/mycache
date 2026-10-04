#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mycache {

class AofWriter;

namespace aof {

// ---- 文件级格式 ----

// 文件头 magic（8 字节），用于识别 AOF 文件与版本。
inline constexpr char kMagic[] = "MYAOF001";
constexpr size_t kMagicSize = 8;

// 一条记录的头：payload 字节数 + payload 的 CRC32（均按小端序存储）。
struct RecordHeader {
  uint32_t len = 0;
  uint32_t crc = 0;
};

// CRC32（IEEE 802.3，与 zlib 一致）。
uint32_t Crc32(const void* data, size_t len);

// 把一条命令（名称 + 字符串参数）编码为 RESP 数组字节流（载荷，不含记录头）。
std::string EncodeCommand(const std::string& name, const std::vector<std::string>& args);

// 把一条 RESP 命令载荷打包为一条完整记录：[len][crc][payload]。
std::string EncodeRecord(const std::string& payload);

// 若 aof 非空，把 name+args 编码为一条记录并追加（AOF 未开启时是空操作）。
// 返回 0 成功，非 0 = errno（透传 AofWriter::Append）。
int LogCommand(AofWriter* aof, const std::string& name, const std::vector<std::string>& args);

// 变参模板：接受单个字符串参数（SET/HSET/PEXPIREAT/LPOP/RPOP 等）。null 早退在构造
// vector 之前——AOF 未开启时避免「把 args 打包成 vector<string>」的浪费（callgrind 实测
// SET 路径该打包占 ~3% 总指令）。SFINAE 约束每个实参可构造 std::string，把
// std::vector<std::string> 排除掉，保证 DEL/LPUSH/RPUSH/HDEL 等扇出场景走非模板重载。
template <typename... Args,
          std::enable_if_t<(std::is_constructible_v<std::string, Args&&> && ...), int> = 0>
int LogCommand(AofWriter* aof, const std::string& name, Args&&... strs) {
  if (aof == nullptr) return 0;
  std::vector<std::string> v;
  v.reserve(sizeof...(Args));
  (v.emplace_back(std::forward<Args>(strs)), ...);
  return LogCommand(aof, name, v);
}

}  // namespace aof
}  // namespace mycache
