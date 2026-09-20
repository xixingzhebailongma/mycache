#pragma once

#include <cstddef>

namespace mycache {

class CommandRouter;

// 子进程 buffer 大小（字节）。rewrite 出的单条记录（一个 key 的 SET/HSET 或一个
// list 分块）不得超过 rec 缓冲；超限会失败（_exit(1)）。1 MiB 对学习项目的
// key/value 规模足够。
constexpr size_t kRewriteOutCap = 1u << 20;  // 输出流缓冲
constexpr size_t kRewriteRecCap = 1u << 20;  // 单条记录 payload 暂存

// AOF rewrite 子进程入口：把 router 各 shard 的当前状态（COW 快照）写成紧凑 AOF 到
// temp_path。out_buf/rec_buf 是 fork 前由父进程预分配的缓冲，本函数全程不做任何
// 堆分配、不加锁，只做 ::open/::write/::fsync/::close/::_exit，从而在多线程进程
// fork 后安全执行。本函数只在子进程中调用且永不返回（成功 _exit(0)，失败 _exit(1)）。
void RewriteAofChild(CommandRouter* router, const char* temp_path,
                     char* out_buf, size_t out_cap,
                     char* rec_buf, size_t rec_cap);

}  // namespace mycache
