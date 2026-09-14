# 内存治理：maxmemory 与采样淘汰

本文讲 Week 5 内存治理的「是什么 / 为什么 / 取舍」。配套代码：

- `include/mycache/eviction/eviction_policy.h` — 策略枚举与配置
- `include/mycache/eviction/lru.h` + `src/eviction/lru.cpp` — 缓存的 LRU 时钟
- `include/mycache/eviction/sampler.h` + `src/eviction/sampler.cpp` — 随机采样
- `src/storage/db/shard.cpp` — `EvictIfNeeded` / `EvictOne` / `PickVictim`
- `src/monitor/info.cpp` — `INFO memory` / `keyspace`

---

## 1. 一条命令的淘汰决策链

```
客户端 SET k v
   └─► DispatchCommand（IO 线程）路由到 ShardWorker
         └─► SetCommand 先调 ctx.db->EvictIfNeeded()   // 写前检查
               ├─ maxmemory == 0          → 直接放行
               ├─ policy == noeviction     → used_memory() > maxmemory ? 回 OOM : 放行
               └─ lru / random             → while (used_memory() > maxmemory) EvictOne()
                                               EvictOne = 采样 N 个 key → 挑最旧/随机 → DeleteKey
         └─ 放行后 ctx.db->Set(...) 才真正写入
```

关键点：**检查发生在写之前**，`noeviction` 超限时根本不写，直接回
`-OOM command not allowed when used memory > 'maxmemory'`；LRU/random 则是「先淘汰腾出
空间，再写入」，与 Redis `processCommand` 里 `performEvictions` 的语义一致。

---

## 2. 为什么「按 shard 均分」等于一个全局水位

内存计数只有一份：`used_memory()`（跟踪分配器，原子）。key 按
`DictHash(key) % shard_count` 均匀散到各 shard，所以每个 shard 占用的内存近似
`used_memory() / shard_count`。给每个 shard 的配额是 `maxmemory / shard_count`，于是

```
本 shard 超限  ⇔  used_memory()/N  >  maxmemory/N  ⇔  used_memory() > maxmemory
```

两边同除以 shard 数，得到同一个全局比较。因此**每个 shard 只需在自己的写路径上检查
全局 `used_memory()`，并从自己的 key 里淘汰**，就同时实现了「全局内存到上限自动淘汰」
和「淘汰工作量按 shard 均分」——不需要再造一个会漂移的 per-shard 内存计数器。

**取舍（要能讲清楚）**：这是「按 shard 均分」的近似。极端情况下，一个 shard 被写入
热点打满、而别的 shard 很冷时，热点 shard 只能淘汰自己的 key，可能把自己的 key 删光
也压不回全局水位，于是回 OOM——尽管别的 shard 还空着很多。缓解手段：哈希均衡（key 均
匀分布）、每个 shard 都触发（写落到哪个 shard 哪个 shard 淘汰）。真正的「全局统一淘汰
池」要跨线程协调，代价是引入锁/协调开销，与本项目「shard 无锁」的卖点相悖。

---

## 3. 近似 LRU：采样而不是真链表

精确 LRU 需要维护全局双向链表 + 哈希（`unordered_map` + `list`），每次访问 O(1) 移动
节点，但代价是：
- 每个 key 多两个指针 + 一个链表节点；
- 每次读都要写（移动节点），缓存不友好；
- 在并发/分片下全局链表本身成了争用点。

Redis 的方案（也是本项目采用的）是**近似 LRU**：给每个对象记一个 `lru` 时间戳，淘汰时
**随机采样 N 个 key（`maxmemory_samples`，默认 5），比较它们的 lru，淘汰最旧的**。采样
数越大越接近精确 LRU，但每次淘汰要遍历更多条目。这个「采样 N 个、淘汰最旧」就是任务书
里 D1 的核心。

本项目与 Redis 的细微差别：
- Redis 用 24 位 lru 时钟（约 194 天回绕），存进 `robj`；我们用 `uint64_t` 毫秒，免去
  回绕处理（学习项目求简单）。
- Redis 的 `dictGetSomeKeys` 采样；我们的 `SampleDictEntries` 同样随机挑桶、沿链收集，
  rehash 期间两张表都参与。

**缓存的 LRU 时钟**：如果每次访问都调 `system_clock::now()`（`UnixMs`），GET 热路径会
多一次时钟读取。为此 `LruClock()` 读一个全局原子，由 shard 的 100ms 定时器周期刷新
（`RefreshLruClock`），等价于 Redis `serverCron` 每 100ms 刷 `server.lruclock`。首次调用
自初始化，单测不依赖后台线程也能拿到非零时钟。

**采样可能够不到少量残余 key**：采样是随机挑桶，表很大而 key 只剩几个时，连续挑到空桶
可能采样失败、淘汰提前终止。Redis 同样有这个问题（`dictGetSomeKeys` 有 `maxsteps` 上
限），后果都是「没法精确压到最后一个字节」。实际水位上限远高于「空表」残留时无感；单测
里因此不要求「删到空」，只验证「确实淘汰了、内存下降了」。

---

## 4. 五种策略

| 策略 | 采样范围 | 淘汰对象 |
|------|---------|---------|
| `noeviction` | — | 不淘汰，超限回 OOM |
| `allkeys-lru` | `dict_`（全部 key） | lru 最小（最久未访问） |
| `allkeys-random` | `dict_` | 随机 |
| `volatile-lru` | `expires_`（带 TTL 的 key） | lru 最小 |
| `volatile-random` | `expires_` | 随机 |

`volatile-*` 只淘汰设了 TTL 的 key：如果全是无 TTL 的 key，`volatile-lru/random` 在超限
时**无 key 可淘汰**，最终回 OOM（和 Redis 一致——这是配置语义，不是 bug）。

---

## 5. 统计口径（INFO 怎么算）

- `used_memory` — 跟踪分配器累计的堆占用（`malloc_usable_size` 之和）。
- `mem_fragmentation_ratio` = `RSS / used_memory`。RSS 从 `/proc/self/statm` 读。注意：
  数据集很小时该值会虚高（RSS 含线程栈、libc 等 ~MB 级，`used_memory` 只有几百字节），
  数据量上来后趋向 ~1.5–2。
- `keyspace_hits` / `keyspace_misses` — 在 `GET`/`EXISTS` 命令处理里计数。
- `evicted_keys` / `expired_keys` — 分别在 `EvictOne` 删除、`ExpireIfNeeded`/主动过期
  删除时计数。
- `db0:keys / expires` — 各 shard 原子计数器 `key_count_`/`expire_count_` 之和，跨线程
  安全。

---

## 6. 自查题

1. 为什么淘汰检查要放在「写之前」而不是「写之后」？`noeviction` 写之后检查会怎样？
2. 「按 shard 均分」为什么能化简成一个全局 `used_memory() > maxmemory` 的判断？代价是什么？
3. 近似 LRU 采样为什么采样数越多越准？`maxmemory_samples` 设成 1 会怎样？
4. `LruClock()` 为什么要缓存，而不是每次访问直接读系统时钟？
5. `volatile-lru` 在全库无 TTL 的情况下超限会怎样？
6. 为什么 list 用 `zmalloc` 手写双向链表，而不是 `std::deque<std::string>`？
