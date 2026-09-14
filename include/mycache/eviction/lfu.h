#pragma once

// LFU（Least Frequently Used）淘汰策略在 Week 5 不实现，当前仅支持
// noeviction / allkeys-lru / allkeys-random / volatile-lru / volatile-random，
// 见 eviction_policy.h。LRU 的近似实现（采样淘汰 + 缓存时钟）见 lru.h 与
// sampler.h。
