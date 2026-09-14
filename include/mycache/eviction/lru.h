#pragma once

#include <cstdint>

namespace mycache {

// 近似的 LRU 时钟（毫秒）。为省去每次 key 访问都读系统时钟的开销，用一个全局
// 原子缓存当前毫秒值，由后台定时器（shard worker 的 100ms 到期/淘汰周期）刷新
// —— 与 Redis 的 serverCron 每 100ms 刷新 server.lruclock 一致。
//
// 首次调用时若尚未初始化（值为 0），会立即读一次墙钟并写回，保证单测里不依赖
// 后台线程也能得到非零时钟。
uint64_t LruClock();

// 用当前墙钟刷新缓存时钟（由定时器周期调用；测试也可手动调用以推进时间）。
void RefreshLruClock();

}  // namespace mycache
