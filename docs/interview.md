# 简历 & 面试准备（Week 5）

## 1. 简历 bullet（可直接裁剪）

**mycache —— 从零实现的 Redis 兼容内存 KV 服务（C++17）**

- 实现 Reactor 网络层：epoll 边沿触发 + 主从线程模型（accept + N 个 IO 线程 + M 个 shard
  worker），流水线批量回包，`redis-benchmark` GET/SET/INCR/LPUSH 达 **13 万 QPS、亚毫秒
  P99**（同机原生 Redis 的 ~60%）。
- 存储引擎：SDS 二进制安全字符串、渐进式 rehash 的哈希表（双表 + 逐步迁移）、引用计数
 对象（int/string/list/hash 编码），百万 key 压测内存稳定。
- 内存治理：实现 5 种 maxmemory 淘汰策略（noeviction/allkeys-lru/random/volatile-*），
  **近似 LRU 采样淘汰**（采样 N 个 key 淘汰最旧）+ 缓存 LRU 时钟，到上限自动淘汰不 OOM。
- 持久化：AOF 追加日志（always/everysec/no 三档 fsync）+ CRC32 校验 + 半条记录截断修复，
  `kill -9` 不丢数据。
- 工程化：INFO 命令（内存/碎片率/命中率）、ASan/UBSan + valgrind 零泄漏、2–4h 长稳测试、
  单元/集成/崩溃恢复三级测试。

**一句话版本**：用 C++17 从零实现 Redis 子集，覆盖 Reactor 网络、无锁分片存储、渐进
rehash、LRU 采样淘汰、AOF 崩溃恢复，压测 13 万 QPS、亚毫秒 P99。

## 2. 项目难点与取舍

| 难点 | 取舍 |
|------|------|
| 分片后内存怎么「按 shard 均分」 | 只维护一个全局 `used_memory()`；因 key 哈希均匀，per-shard 配额与用量同除以 N 后等价于全局水位，省掉会漂移的 per-shard 计数器。代价：热点 shard 可能自删到空仍回 OOM |
| 精确 LRU vs 近似 LRU | 选近似采样（Redis 同款）：免去全局链表 + 每次读都写的开销，采样 N 个淘汰最旧 |
| LRU 时钟每次访问读系统时钟太贵 | 缓存成全局原子，100ms 定时器刷新（仿 Redis serverCron）|
| list 用 std::deque 会绕过内存统计 | 用 zmalloc 手写双向链表，让 used_memory 精确 |
| AOF 多 shard 并发追加会交错 | 单 AofWriter + 互斥锁；always 持锁 fsync 是吞吐杀手，选 everysec |
| INFO 跨线程统计 | 每 shard 原子计数 key/expire，避免跨线程加锁读 dict |

## 3. 面试题与参考答案（要点）

**Q1 Reactor / 事件循环**：epoll 边沿触发 + `eventfd` 唤醒 + pending 队列。accept 用主
线程，读写在 IO 线程，命令路由到 shard worker；跨线程用 `RunInLoop` 保证同一 key 串行。
ET 必须读到 EAGAIN 才算读干净，写也要写满。

**Q2 渐进式 rehash**：双哈希表 ht[0]/ht[1]，扩容时分配 ht[1]、`rehashidx=0`，之后每次写
操作迁移一个桶（`RehashStep`），读查两张表，直到 ht[0] 搬空再交换。好处是**分摊**，不会
像一次性 rehash 那样阻塞服务。分片后 Shard 读路径用 `FindAndRehash` 也推进 rehash（对齐
Redis `dictFind`）；只有 Week1 的 `Keyspace` 共享锁读路径保持纯只读。

**Q3 TTL**：惰性删除（每次查 key 先看是否到期）+ 主动过期（timerfd 每 100ms 随机采样）。
存**绝对时间戳**（`UnixMs` 墙钟）而非相对，这样 AOF 里记 `PEXPIREAT` 重放结果确定——
`EXPIRE key 100` 要翻译成 `PEXPIREAT key <now+100s>`，否则重启后重放会重新计时。

**Q4 AOF 三种 fsync**：`write()` 都进页缓存，区别只在 fsync 时机。always 每条 fsync；
everysec 后台线程每秒 fsync；no 不 fsync。`kill -9` 三种都不丢（数据在页缓存），fsync 决定
的是**断电**时的持久性。每条记录 `[len][crc32][payload]`，靠长度 + 校验和能确定性判断半条
记录，截断修复。

**Q5 Shard Worker**：key 按哈希路由到固定 worker，同一 key 的命令在单线程上 FIFO 执行，
因此**存储无需加锁**；对 key 的并发访问天然串行化。代价是跨 shard 的多 key 命令（MGET）
要协调多个 worker，本项目暂未实现。

**Q6 LRU 采样淘汰**：给对象记 lru 时间戳，淘汰时随机采样 N 个 key（`maxmemory_samples`），
比较 lru 淘汰最旧。近似但 O(N) 可调，避免了精确 LRU 的全局链表 + 每次读写节点的开销。

**Q7 锁竞争**：存储层无锁（分片）；唯一的写路径争用是 AOF 单锁，`always` 持锁 fsync 会
串行化所有 shard 的写。分析 → 用 `everysec`。如何定位：`perf` / 对比开关 AOF 的吞吐。

**Q8 为什么 P99 亚毫秒、QPS 13 万**：深度流水线 + 无锁分片 + 内存操作；瓶颈在
内存分配与哈希，不在锁。注意 benchmark 是流水线口径，单条 RTT 受网络主导。
