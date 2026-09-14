#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
void LogCommand(AofWriter* aof, const std::string& name, const std::vector<std::string>& args);

}  // namespace aof
}  // namespace mycache
