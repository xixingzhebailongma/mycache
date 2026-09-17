# Week 5 代码阅读指南：内存治理 + 性能稳定性 + 工程化

本文是配套 `docs/eviction.md`（讲「是什么/为什么」）的**代码阅读地图**，讲的是
「代码在哪、按什么顺序读、每段在干什么」。读完并做完末尾的自查题，就能真正掌握这一周
的改动。

---

## 1. 改动清单

### 新增（核心）
| 文件 | 作用 |
|------|------|
| `include/mycache/eviction/eviction_policy.h` + `src/eviction/eviction_policy.cpp` | `EvictionPolicy` 枚举、`EvictionConfig`、`ParseEvictionPolicy` |
| `include/mycache/eviction/lru.h` + `src/eviction/lru.cpp` | 缓存的 `LruClock()` / `RefreshLruClock()` |
| `include/mycache/eviction/sampler.h` + `src/eviction/sampler.cpp` | `SampleDictEntries`（随机桶采样） |
| `include/mycache/monitor/metrics.h` + `src/monitor/metrics.cpp` | `Metrics` 单例（命中/淘汰/过期/命令/连接计数） |
| `include/mycache/monitor/info.h` + `src/monitor/info.cpp` | `BuildInfo`（INFO 命令文本） |
| `include/mycache/storage/collection/list.h` + `src/storage/collection/list.cpp` | zmalloc 双向链表（list 类型） |
| `src/command/server_commands.cpp` | `INFO` / `DBSIZE` |
| `src/command/list_commands.cpp` | `LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN` |

### 新增（测试）
| 文件 | 作用 |
|------|------|
| `tests/unit/eviction_test.cpp` | 5 种策略、淘汰顺序、计数器一致性 |
| `tests/unit/list_test.cpp` | list 增删查、WRONGTYPE、空表删 key |
| `tests/unit/info_test.cpp` | `ParseMemorySize`、`BuildInfo` 冒烟 |
| `tests/unit/lru_test.cpp` | LRU 时钟自初始化、采样 |

### 修改
| 文件 | 改动 |
|------|------|
| `include/mycache/common/config.h` | 新增 `maxmemory`/`maxmemory_policy`/`maxmemory_samples` + `ParseMemorySize` |
| `include/mycache/storage/object/object.h` | 新增 `uint64_t lru` 字段、`IsList()` |
| `src/storage/object/string_object.cpp` / `src/storage/collection/hash.cpp` | 创建对象时初始化 `lru` |
| `include/mycache/storage/object/encoding.h` | 新增 `kLinkedList` 编码 |
| `src/storage/object/object.cpp` | `DecrRefCount` 增加 list 分支 |
| `include/mycache/storage/db/shard.h` + `.cpp` | 淘汰逻辑 + 原子计数 `key_count_/expire_count_` + list 方法 |
| `include/mycache/command/command_context.h` | 新增 `config`/`router` 指针 + `EvictOrOom` 辅助 |
| `src/command/string_commands.cpp` | `INCR/DECR/INCRBY/DECRBY`；`SET` 写前淘汰；`GET` 计命中 |
| `src/command/generic_commands.cpp` | `EXISTS` 计命中/未命中 |
| `src/command/hash_commands.cpp` | `HSET` 写前淘汰 |
| `src/server/server.cpp` | 装配淘汰配置、命令计数、context 带 config/router |
| `src/server/command_router.cpp` / `shard_worker.cpp` | 传递 `EvictionConfig`；定时器刷新 LRU 时钟 + 后台兜底淘汰 |
| `CMakeLists.txt` | 挂载新测试；`CMAKE_EXPORT_COMPILE_COMMANDS ON` |

---

## 2. 一张图看懂两条数据流

### 写入淘汰路径（运行时）
```
客户端 SET k v
   └─► DispatchCommand ──► 路由到 ShardWorker
          └─► SetCommand：EvictOrOom(ctx)          // command_context.h:36
                └─► ctx.db->EvictIfNeeded()        // shard.cpp:30
                      ├─ noeviction & 超限 → false → 回 OOM 错误
                      └─ lru/random → while(used_memory()>maxmemory) EvictOne()  // shard.cpp:46
                            └─► SampleCandidates()  // 采样 N 个 key（shard.cpp:55）
                            └─► PickVictim()        // lru 挑最旧 / random 随机（shard.cpp:71）
                            └─► DeleteKey()         // 删 dict_ + expires_，减计数（shard.cpp:102）
          └─► 放行后 ctx.db->Set(...) 才真正写入
```

### INFO 统计路径（无 key，IO 线程直接执行）
```
客户端 INFO
   └─► InfoCommand（server_commands.cpp）→ BuildInfo(config, router)（info.cpp:60）
         ├─ used_memory()（跟踪分配器）+ ResidentMemoryBytes()（/proc/self/statm）
         ├─ Metrics 单例（命中/淘汰/过期/命令/连接）
         └─ 汇总各 shard 的原子计数 Size()/ExpireCount()（shard.cpp:465/469）
```

---

## 3. 推荐阅读顺序（由里到外）

1. **先懂配置**：`config.h` 的 `maxmemory` 三字段 + `ParseMemorySize`（为什么支持 `kb/mb/gb`）
2. **再懂策略**：`eviction_policy.h/.cpp`（5 种策略、`EvictionConfig`）
3. **再懂时钟**：`lru.h/.cpp`（为什么要缓存 LRU 时钟）
4. **再懂采样**：`sampler.h/.cpp`（随机桶采样、如何处理 rehash）
5. **再懂对象**：`object.h` 的 `lru` 字段 + `string_object.cpp`/`hash.cpp` 的初始化
6. **核心**：`shard.cpp` 的 `EvictIfNeeded/EvictOne/SampleCandidates/PickVictim/DeleteKey`
7. **再懂 list**：`list.cpp`（为什么手写链表 + 那个 `prev/next` 的坑）
8. **再懂统计**：`metrics.cpp` + `info.cpp`
9. **再看触发点**：`command_context.h` 的 `EvictOrOom` + `string_commands.cpp`/`hash_commands.cpp`
10. **最后串起来**：`server.cpp` / `command_router.cpp` / `shard_worker.cpp`
11. **验证**：`tests/unit/{eviction,list,info,lru}_test.cpp` + `scripts/{acceptance,eviction_test,soak_test}.sh`

---

## 4. 关键代码逐段讲解

### 4.1 缓存 LRU 时钟 — `lru.cpp:13`

```cpp
uint64_t LruClock() {
  uint64_t c = g_lru_clock.load(std::memory_order_relaxed);
  if (c == 0) {
    c = static_cast<uint64_t>(UnixMs());
    g_lru_clock.store(c, std::memory_order_relaxed);
  }
  return c;
}
```

> 为什么缓存：`GET` 每次访问都要更新 `o->lru`（见 `shard.cpp` 的 `LookupValue`），如果
> 每次都调 `system_clock::now()`，热路径就多一次时钟读取。缓存成一个全局原子后，读它只是
> 一次 `load`。`c == 0` 是「首次自初始化」——保证单测不依赖后台线程也能拿到非零时钟。
> `RefreshLruClock()`（`lru.cpp:22`）由 shard 定时器每 100ms 调用，仿 Redis `serverCron`。

### 4.2 采样 — `sampler.cpp:7`

```cpp
size_t size0 = d->Table(0).size;
size_t size1 = d->Rehashing() ? d->Table(1).size : 0;
size_t total = size0 + size1;
...
size_t max_steps = count * 10;   // 上限，避免稀疏表里空转太久
```

> rehash 期间条目分布在两张表，所以 `total = size0 + size1`；随机挑 `idx`，落在前半段查
> `ht[0]`、后半段查 `ht[1]`。`max_steps = count*10` 是「最多尝试多少次随机挑桶」，避免在
> 稀疏表里一直命中空桶（这就是 Q11 里「采样可能够不到残余 key」的来源）。

### 4.3 淘汰决策 — `shard.cpp:30` / `:46` / `:71`

```cpp
bool Shard::EvictIfNeeded() {
  if (eviction_.maxmemory <= 0) return true;              // 不限制
  if (eviction_.policy == EvictionPolicy::kNoEviction) {
    return used_memory() <= limit;                        // 超限直接拒绝
  }
  while (used_memory() > limit && max_iter-- > 0) {
    if (!EvictOne()) break;                               // 没有可淘汰的 key
  }
  return used_memory() <= limit;
}
```

```cpp
sds Shard::PickVictim(const std::vector<DictEntry*>& candidates) {
  if (policy == kAllkeysLru || policy == kVolatileLru) {
    for (auto* e : candidates) {
      Object* o = (policy == kAllkeysLru)
        ? static_cast<Object*>(e->val)         // allkeys：e 来自 dict_，val 就是对象
        : /* volatile：e 来自 expires_，val 是到期时间，得再去 dict_ 查 */ ...;
      if (o->lru < best) { best = o->lru; victim = e; }
    }
  }
}
```

> 关键点：`allkeys-*` 采样的是 `dict_`（val 就是 `Object*`），而 `volatile-*` 采样的是
> `expires_`（val 是到期时间戳），所以 volatile-lru 要再 `dict_.Find(e->key)` 拿对象读 lru。

### 4.4 原子计数 — `shard.cpp:102` / `:146`

```cpp
bool Shard::DeleteKey(sds key) {
  bool existed = dict_.Delete(key);
  if (existed) key_count_.fetch_sub(1, std::memory_order_relaxed);
  if (expires_.Delete(key)) expire_count_.fetch_sub(1, std::memory_order_relaxed);
  return existed;
}
```

```cpp
void Shard::Set(...) {
  if (expires_.Delete(k)) expire_count_.fetch_sub(1, ...);  // 先清 TTL
  if (dict_.Replace(k, o) == 1) key_count_.fetch_add(1, ...); // 新增才加
}
```

> 为什么用原子计数：`INFO` 在 IO 线程汇总各 shard 的 key/expire 数，而各 shard 的 dict
> 只能由它的 worker 线程访问。直接跨线程读 `dict_.Size()` 是数据竞争，所以用原子计数在
> 写路径维护，`INFO` 读原子值就安全。

### 4.5 那个真实的 bug — `list.cpp:10`

```cpp
ListNode* NewNode(sds value) {
  // zmalloc 是 malloc，不会执行 ListNode 的默认成员初始化器，须显式置空 prev/next。
  ListNode* n = static_cast<ListNode*>(zmalloc(sizeof(ListNode)));
  n->prev = nullptr;
  n->next = nullptr;
  n->value = value;
  return n;
}
```

> `ListNode` 结构体写了 `prev = nullptr; next = nullptr;` 的默认成员初始化器，但那**只在
> 用 `new` / 值初始化时生效**；`zmalloc` 底层是 `malloc`，内存是垃圾值。第一版漏了
> `n->next = nullptr`，`ListPushBack` 又只设了 `prev` 不设 `next`，于是尾节点的 `next` 是
> 垃圾指针，`ReleaseListObject` 沿 `next` 遍历时崩掉——ASan 在 `ReleaseListObject` 抓到的
> SEGV。这个 bug 单元测试没抓到（要 `RPush` 两个以上元素 + 析构才触发），是 ASan 暴露的。

### 4.6 INCR 记 AOF — `string_commands.cpp:63` / `:84`

```cpp
if (__builtin_add_overflow(v, delta, &result)) { /* 回溢出错误 */ }
std::string nv = std::to_string(result);
ctx.db->Set(args[1].str, nv);
aof::LogCommand(ctx.aof, "SET", {args[1].str, nv});   // 记绝对结果
```

> 为什么记成 `SET <key> <结果值>` 而不是 `INCR <key>`：我们的 AOF 恢复只认识
> `SET/DEL/HSET/HDEL/PEXPIREAT/LPUSH/RPUSH/LPOP/RPOP`。记绝对结果，重放时无论执行多少次、
> 何时执行，都直接落到同一个值；记 `INCR` 增量则重放时会在「恢复出来的旧值」上再 +1，
> 语义依赖重放顺序，也要求恢复器额外理解 INCR。这是「记录结果、不记录操作」的取舍。

### 4.7 碎片率 — `info.cpp:63`

```cpp
double frag = (used > 0) ? (double)rss / (double)used : 0.0;
```

> `used` 是跟踪分配器累计的引擎堆占用；`rss` 是 `ResidentMemoryBytes()` 读
> `/proc/self/statm` 的整个进程驻留集（含线程栈、libc 等）。数据很少时 `used` 只有几百
> 字节而 `rss` 是 MB 级，比值会虚高到几千；数据量上来后趋向 ~1.5–2。

---

## 5. 自查题（先自己想，再对答案）

### Q1 — LRU 时钟
`LruClock()` 为什么不直接每次读 `system_clock::now()`？`c == 0` 的自初始化分支是干嘛的？

### Q2 — 采样与 rehash
`SampleDictEntries` 为什么要 `size0 + size1`？如果 rehash 进行中、条目在两张表上，只采样
`ht[0]` 会漏掉什么？

### Q3 — 按 shard 均分
为什么「每个 shard 检查全局 `used_memory() > maxmemory`」就等价于「按 shard 均分」？代价
是什么？

### Q4 — noeviction 的检查时机
`EvictIfNeeded` 为什么放在写**之前**调用？如果放到写之后（`SET` 已经写入再检查超限），
`noeviction` 会有什么问题？

### Q5 — volatile 的二次查找
`volatile-lru` 的 `PickVictim` 为什么不能直接用 `e->val` 当对象读 lru，而要再
`dict_.Find(e->key)`？

### Q6 — 那个 bug
`ListNode` 明明写了 `prev = nullptr; next = nullptr;`，为什么 `zmalloc` 出来的节点这两个
字段还是垃圾值？由此引发了什么崩溃？

### Q7 — list 为什么手写
为什么不用 `std::deque<std::string>` 存列表？手写 zmalloc 双向链表解决了什么、付出了什么？

### Q8 — INFO 的线程安全
为什么 `INFO` 汇总各 shard 的 key 数要用原子计数 `key_count_`，而不是直接跨线程调
`dict_.Size()`？

### Q9 — Set 里的顺序
`Set` 里「先 `expires_.Delete(k)` 再 `dict_.Replace(k, o)`」的顺序能反过来吗？两个计数
（`expire_count_`、`key_count_`）分别在什么条件下才增减？

### Q10 — INCR 与 AOF
`INCR` 为什么 AOF 记 `SET key <结果>` 而不是 `INCR key`？记增量会有什么问题？

### Q11 — 淘汰的边界
`EvictIfNeeded` 的 `while` 循环为什么要有 `max_iter` 上限？`EvictOne()` 返回 false 说明
什么？这和采样有什么关系？

### Q12 — 碎片率虚高
`mem_fragmentation_ratio` 为什么在数据很少时会是几千（比如 8804.72），数据多了才趋向 1~2？

---

## 6. 答案

### A1
因为 `GET` 每次命中都要在 `LookupValue`（`shard.cpp`）里写 `o->lru = LruClock()`，热路径上
每次读系统时钟（`std::chrono::system_clock::now()`）是有成本的。缓存成全局原子后，读时钟
退化成一次 `load`。`c == 0` 分支是「首次自初始化」：进程刚启动、定时器还没跑时，第一次
调用 `LruClock()` 也要能返回非零值（否则单测里所有对象的 lru 都是 0，淘汰顺序没意义）。
`RefreshLruClock()` 由 shard 的 100ms 定时器调用，把真实时间写回缓存。

### A2
因为渐进式 rehash 期间，条目被分散在 `ht[0]` 和 `ht[1]` 两张表上（旧的还没搬完，新的正在
接收）。只采样 `ht[0]` 会漏掉已经迁移到 `ht[1]` 的那部分 key，采样结果偏向「还没被迁移的
key」，淘汰就不公平了。所以 `total = size0 + size1`，随机下标落在前半段查 `ht[0]`、后半段
查 `ht[1]`。

### A3
内存只有一份：`used_memory()`。key 按 `DictHash(key) % shard_count` 均匀散到各 shard，所以
每个 shard 占的内存约等于 `used_memory() / N`，配额是 `maxmemory / N`。两边同除以 N，就得到
同一个全局判断 `used_memory() > maxmemory`。代价是「均分」是近似的：某个 shard 被写热点打
满、别的 shard 很冷时，热点 shard 只能删自己的 key，可能删光了也压不回全局水位，于是回
OOM——即便别的 shard 还空着。真正的全局淘汰池要跨线程协调，与本项目「shard 无锁」相悖。

### A4
`noeviction` 的语义是「内存超限时**拒绝**会让内存变大的写」。如果放到写之后检查，`SET`
已经写进去、内存已经超了，这时再决定「拒绝」就晚了——数据已经落地，只能回滚或假装没发生，
都很别扭。放写之前：先看水位，超限就直接回 OOM、不碰数据。LRU/random 则反过来是「先淘汰
腾出空间，再写入」，所以也必须在写之前跑完淘汰。

### A5
`volatile-*` 的 `SampleCandidates` 采样的是 `expires_` 这张 dict，它的 `val` 存的是**到期
时间戳**（`intptr_t` 强转的 `when_ms`），不是 `Object*`。所以要拿到对象的 lru，必须拿采样
条目里的 key 再去 `dict_` 里 `Find` 一次，取出真正的 `Object*`。`allkeys-*` 采样的是 `dict_`
本身，`e->val` 直接就是对象，就不用二次查找。

### A6
`zmalloc` 底层是 `std::malloc`，只分配不初始化，内存是垃圾值；结构体里的 `= nullptr` 是
**默认成员初始化器**，只在「默认构造」（`new ListNode` / 值初始化 `ListNode{}`）时执行，
对 `malloc` 出来的裸内存无效。第一版 `NewNode` 只设了 `value`，`ListPushBack` 又只设
`n->prev` 没设 `n->next`，于是尾节点的 `next` 是垃圾指针；`ReleaseListObject` 沿 `next`
链遍历释放时读到垃圾指针，ASan 报 `ReleaseListObject` 处 SEGV。修复就是显式把 `prev/next`
都置空。教训：**手写分配器（zmalloc）+ 裸结构体时，默认初始化器不可靠，字段要显式赋值**。

### A7
为了**内存统计的正确性**。`used_memory()` 只统计经过 `zmalloc` 的内存；`std::deque` 内部用
标准分配器 `operator new`，绕过了跟踪分配器，列表占的内存就「消失」了——`INFO memory` 会对
不上、淘汰时 `used_memory()` 也不随列表删除下降。手写 zmalloc 双向链表让每个节点和值都被
计数，代价是多写一个简单的链表结构（几行 `push/pop`）。这是「工程正确性 > 少写代码」的取舍。

### A8
`dict_` 是**非线程安全**的，只有它归属的 `ShardWorker` 线程能访问。`INFO` 在 IO 线程执行，
直接调 `dict_.Size()`（读 `ht[0].used + ht[1].used`）会和 worker 线程的并发写构成数据竞争
（未定义行为，可能读到中间态）。所以在写路径（`Set/Del/DeleteKey/ExpireAt/HSet/HDel`）同步
维护两个 `std::atomic<size_t>`，`INFO` 只 `load` 原子值，跨线程安全且几乎零开销。

### A9
不能反过来。`SET` 覆盖一个已有 TTL 的 key 时要清掉它的 TTL（与 Redis 一致），所以先
`expires_.Delete(k)`，删除成功才 `expire_count_--`。然后 `dict_.Replace(k, o)` 返回 1 表示
「新增 key」（`AddRaw` 建了新条目），此时 `key_count_++`；返回 0 表示「覆盖旧值」，key 数
不变。如果反过来先 `Replace`，旧值已经被释放、新值已写入，再清 expires 逻辑上也能做，但
两个 dict 的先后顺序会破坏「Delete 返回是否删除成功」这个判断所需的原子性——更关键的是
`expires_.Delete` 必须在写 `dict_` 之前，否则覆盖一个本来带 TTL 的 key 会留下一个悬空的
expire 条目（key 在 dict_ 里换了对象，expires_ 里还留着旧 TTL）。

### A10
记增量（`INCR key`）在 AOF 重放时是**相对**操作：重放器在「恢复出来的旧值」上再 +1。这不
仅要求恢复器额外理解 INCR 的语义，还依赖「恢复顺序 = 原始写入顺序」这个隐含前提；一旦某
条记录被截断或顺序调整，结果就偏了。记绝对结果（`SET key <当前值>`）是幂等的——无论重放
几次、何时重放，都直接落到同一个值，和 Week 4 里 `EXPIRE → PEXPIREAT` 记绝对时间的道理
完全一致。

### A11
`EvictOne()` 返回 false 表示「采样没采到任何可淘汰的 key」。采样是随机挑桶，当表很大而剩
余 key 很少时，连续挑到空桶会导致 `SampleDictEntries` 采不到东西（`max_steps = count*10`
设了尝试上限），淘汰就提前终止。`max_iter` 则是外层循环的保护，防止极端情况（比如
`maxmemory` 被设成 1 字节、永远压不到）下死循环。后果是「没法精确压到最后一个字节」——
真实场景水位上限远高于空表残留时无感，单测里因此只断言「确实淘汰了、内存下降了」，不要求
删到空。

### A12
`mem_fragmentation_ratio = RSS / used_memory`。`used_memory` 只算跟踪分配器里的引擎内存
（几个 key 时可能才几百字节），而 `RSS` 是整个进程的驻留集，包含线程栈（默认每线程 ~8MB）、
libc、epoll 结构等，最少也有几 MB。分子是 MB 级、分母是几百字节，比值自然到几千。数据量
上来后 `used_memory` 涨到几十/几百 MB，线程栈等固定开销被摊薄，比值就趋向 1.5~2。这和
Redis 的 `mem_fragmentation_ratio` 口径一致，刚启动的 Redis 也偏高。

---

## 7. 快速自测对照表

| 如果你能答对… | 说明你理解了… |
|---------------|---------------|
| Q1、Q6 | 缓存时钟 + 手写分配器/裸结构体的坑 |
| Q2、Q5、Q11 | 采样淘汰 + 渐进 rehash + 近似 LRU 的边界 |
| Q3、Q4 | shard 均分 + 淘汰检查时机 |
| Q7、Q8 | 内存统计口径 + 跨线程安全 |
| Q9、Q10 | 计数/持久化的语义正确性 |
| Q12 | INFO 碎片率的口径 |
