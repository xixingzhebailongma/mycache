#pragma once

#include <cstddef>
#include <cstdint>

namespace mycache {

class CommandRouter;

// ---- RDB 简化二进制格式 ----
//   [magic "MYRDB001" (8)][version 0x01 (1)]
//   { [record: len(4 LE)][crc(4 LE)][payload] ... }   // 每个 key 一条记录
//   [EOF 0xFF (1)]
// 记录 payload：
//   [type (1)][has_ttl (1)][ttl_ms (8 LE, 若 has_ttl)][key_len (4 LE)][key]
//   [value]  // 见各 type
//     string: [val_len (4 LE)][val]   // val 为逻辑值字节（int 用十进制，string 用 sds）
//     hash:   [field_count (4 LE)] { [f_len (4 LE)][f][v_len (4 LE)][v] }*
//     list:   [elem_count (4 LE)] { [e_len (4 LE)][e] }*
inline constexpr char kRdbMagic[] = "MYRDB001";
constexpr size_t kRdbMagicSize = 8;
constexpr uint8_t kRdbVersion = 0x01;
constexpr uint8_t kRdbEof = 0xFF;
constexpr uint8_t kRdbTypeString = 0;
constexpr uint8_t kRdbTypeHash = 1;
constexpr uint8_t kRdbTypeList = 2;

// 快照缓冲大小。单条 key 记录（key+value 序列化后）不得超过 rec 缓冲。
constexpr size_t kRdbOutCap = 1u << 20;  // 输出流缓冲
constexpr size_t kRdbRecCap = 1u << 20;  // 单条记录 payload 暂存

// 把 router 各 shard 的当前状态写成 RDB 快照到 path（打开时 O_TRUNC）。全程无堆分配、
// 无锁，因此既能在 SAVE 的同步路径（持快照闸门 exclusive 锁、调用线程）运行，也能在
// BGSAVE 的 fork 子进程里运行。失败返回 false。
bool RdbWriteSnapshot(CommandRouter* router, const char* path,
                      char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap);

// BGSAVE 子进程入口：写 RDB 到 path，成功 _exit(0)，失败 _exit(1)，永不返回。
void WriteRdbChild(CommandRouter* router, const char* path,
                   char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap);

}  // namespace mycache
