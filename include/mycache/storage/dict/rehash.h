#pragma once

namespace mycache {

// 渐进式 rehash 的策略参数。

// 每次 Rehash(n) 最多跳过 n * kRehashEmptyVisitFactor 个空桶，避免在稀疏表上
// 单次调用耗时过长（与 Redis 一致）。
constexpr int kRehashEmptyVisitFactor = 10;

// 每次写操作（Add/Delete/Replace）推进的桶数，保持单次操作延迟有界。
constexpr int kRehashBucketPerStep = 1;

}  // namespace mycache
