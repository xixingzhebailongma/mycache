# mycache 面试必过题库 + 答案要点

> 考前 30 分钟过一遍的冲刺版。分 5 轮，难度递增。每题的「答案要点」是要能用自己的话
> 说出来的目标，不是逐字背诵。最后附一份「高频陷阱清单」——那些面试官顺手就会戳的细节。

## 使用说明

- 先看【总览】里自己的弱轮，优先补。
- 每轮先自己**口头答一遍**，再看要点，比对缺了哪句话。
- 重点不是记住名词，是能在被追问「为什么 / 代价是什么」时接得住。

## 总览（诊断）

| 轮次 | 主题 | 首测得分 |
|---|---|---|
| 1 | 基础 · 主流程 | 3.0 / 10 |
| 2 | 设计取舍 | 2.5 / 10 |
| 3 | 边界与故障 | 2.6 / 10 |
| 4 | 通用系统 / C++ | 4.4 / 10 |
| 5 | 行为面 | 2.5 / 10 |

弱项：**第 2、3、5 轮**（「为什么这么选 + 代价」这一层）。

---

## 第 1 轮 · 基础 · 主流程

### Q1. 完整链路：`redis-cli set foo bar` 到回 `+OK`
**要点**：TCP → 主线程 `Acceptor`（epoll ET + `accept4` 读到 EAGAIN）→ round-robin 交给某个
**IO reactor** → IO 线程的 `Connection` 读排空、`OnMessage` 循环解析（一次可解析多条 = 流水线）
→ `DispatchCommand`（小写化 + 查表 + 校验 arity）→ `DictHash(key) % shard_count` 路由到
**ShardWorker** → 写前 `EvictIfNeeded` → 无锁写 `Shard` → `conn->Send` 发现跨线程、经
`shared_from_this` 回跳到连接所属的 IO loop → 批量 flush `+OK`。
漏掉「accept 主线程」「RESP 解析」「回包跨线程回跳」任一环都算不完整。

### Q2. 为什么无锁是成立的？路由改坏的后果？
**要点**：正确性前提是**路由函数必须确定性**——同一个 key 无论何时算都落到同一个 shard
（同一线程），于是对同一 key 的所有操作被**串行化**，不存在并发读写。
改坏路由 = 同一 key 落到不同线程 = **数据竞争 / 丢更新 / double-free（use-after-free）**，
不是「分不清新建还是修改」这么轻。

### Q3. 为什么渐进式 rehash？读路径为什么不推进？
**要点**：一次性 rehash 让触发它的那条命令停顿几百毫秒（延迟尖峰）；双表 ht[0]/ht[1] +
`rehashidx` 把工作摊到每次操作（每次搬 1 桶）。**读路径一度不推进**是因为 Week1 的 `Keyspace`
用 `shared_mutex`，读者持共享锁必须纯只读（所以 `Dict::Find` 是 `const`）。分片后这条约束
消失——`Shard` 单线程无锁，读路径改走 `Dict::FindAndRehash`（先 `RehashStep` 再查两张表，
对齐 Redis 的 `dictFind`）；const `Find` 只保留给已 deprecated 的 Week1 `Keyspace` 共享锁读
路径。**不推进的后果**：读多写少的负载下 rehash 一旦开始可能永远结束不了，ht[0] 一直占
内存，且每次读都要查两张表——这正是读路径也要推进的原因。

### Q4. `SET foo 100` 的 value 在内存里长什么样？
**要点**：`dict_` 里 `foo(sds) -> Object*`；`SET` 无 TTL 所以不进 `expires_`。`Object` 有
type/encoding/refcount/lru。`100` 被 `TryObjectEncoding` 编码成 **int**：值直接塞进 8 字节
的 `ptr`，省掉单独 malloc 一个 sds。
两个关键澄清：① **int 对象不共享**——没有 Redis 那样的 shared integers 池，两个 key 存 100
是两个独立对象；② **refcount 峰值是 2**（create=1 → dict 接管 incref=2 → 函数末尾本地
decr=1），作用是**所有权转移**，不是「多个 key 共享一块数据」。

---

## 第 2 轮 · 设计取舍

### Q1. TTL 为什么同时做惰性删除和主动过期？
**要点**：只惰性删除的话，一个 `EXPIRE` 后不再被访问的 key 永远不回收 = 变相内存泄漏。
主动过期是后台兜底。
主动过期比 Redis 少两样：① **时间预算**（Redis 每轮限时到点就停）；② **「采样中 >25%
已过期就继续扫」的加速路径**。本项目是固定抽 20 个随机桶。
存**绝对墙钟 ms**（`UnixMs`，非 monotonic），AOF 里记 `PEXPIREAT key <绝对ms>`，重放与时间
无关；存相对秒数会在重启/重放时重新计时。代价：墙钟受 NTP 跳变影响（与 Redis 一致）。

### Q2. AOF 三种 fsync 到底差在哪？
**要点**：三种策略**每次都 `write()` 进 OS 页缓存**，所以 **`kill -9` 三种都不丢数据**
（进程死了 OS 还在、页缓存还在）。fsync 干的是「把页缓存强制刷到磁盘」，管的是**断电 /
OS 崩溃**。
- `always`：每条记录持锁 fsync（在回复前）。
- `everysec`：后台线程每秒 fsync 一次，断电最多丢 1s。
- `no`：不 fsync（优雅退出时除外），断电丢 OS 尚未回写的段。
torn-tail 检测/修复：每条记录 `[len u32][crc32 u32][payload]`；因为每次 append 在互斥锁内
`WriteAll` 完整写、记录不交错，半条记录**只会在文件末尾**。恢复时半截头 / `len` 超 EOF /
**末条** CRC 错 → `ftruncate` 截到上一条完整记录；**中间** CRC 错 → 拒启。

### Q3. 为什么近似 LRU 而不是精确 LRU？
**要点**：精确 LRU 是 `hashmap + 双向链表`，**O(1)，不扫描**。真正的代价是：① 每次读都要
写（移动节点），缓存不友好 + 全局链表成为争用点；② 每 key 多两个指针 + 一个链表节点；③
并发/分片下一条全局链表本身就是瓶颈。
近似 LRU：对象记 `lru` 时间戳，淘汰时采样 N 个淘汰最旧。N 越大，采样越可能包含真正很旧的
key，越接近真实 LRU。**`maxmemory_samples=1` → 退化成 allkeys-random**（无比较对象）。
时钟为什么缓存：每次访问 `clock_gettime` 多一次 syscall；`LruClock()` 读全局
`atomic<uint64_t>`，由 shard 100ms 定时器刷新（仿 Redis `serverCron`），首次调用自初始化。

### Q4. 「按 shard 均分」怎么化简成一个全局水位？
**要点**：每 shard 配额 `maxmemory/N`，因 key 哈希均匀、每 shard 用量 ≈ `used_memory()/N`；
「本 shard 超限」⇔ `used/N > maxmemory/N` ⇔ 同乘 N ⇔ `used_memory() > maxmemory`。
代价（具体场景）：某 shard 被写热点打满、其他 shard 冷时，热点 shard 只能淘汰**自己的** key，
可能删到空也压不回全局水位 → 返回 OOM，尽管别的 shard 还空着。跨线程「全局淘汰池」要协调/加锁，
与本项目「shard 无锁」冲突。
（澄清：「hash 冲突」是 bucket 表里 `hash & sizemask` 的碰撞，和「key 在 shard 间分布不均」
是两码事。）

---

## 第 3 轮 · 边界与故障

### Q1. 跨 shard 多 key 命令的问题
**要点**：`DEL a b c` / `EXISTS a b c` 按 `first_key=1` 只落在 `a` 的 shard，`b`/`c` 若哈希到
别的 shard **根本不会被删/算**，命令却照常返回（还返回错的计数）。
`MGET`/`MSET` 难的不是「不能拆发」（scatter-gather 很简单），是**跨 shard 原子性/一致性**：
各 shard 独立线程、在略微不同时刻执行，无法给出一个一致快照。要一致就得跨 shard 加锁（毁掉
无锁设计）或全局时间戳/MVCC——本质退回单线程。**Redis 的 MGET 免费，是因为它单线程；分片用
并发换来的代价就是跨 key 原子性。**

### Q2. 为什么 INCR 记成 SET、EXPIRE 记成 PEXPIREAT？
**要点**：`INCR` 是「读当前值 +1 写回」，结果依赖当前状态；直接记 `INCR` 重放**非幂等**（重放
两遍 +2、在非空状态上重放值错）。记 `SET <绝对结果>` 让每条记录**幂等且自包含**，从任何状态重放
都收敛到同一值（也为 AOF rewrite 铺路）。
`EXPIRE key 100` 是相对的；记相对形式重启后重新解释成「从重放时刻起 100s」，TTL 被重置。翻译成
`PEXPIREAT <绝对ms>` 把截止时刻冻结，重放与时间无关。

### Q3. 为什么 `always` 塌到 ~180 ops/s？
**要点**：是**锁**不是 syscall 贵。`always` 在**持有 AofWriter 唯一 mutex 的同时**做毫秒级
fsync，7 个 shard 的写全部阻塞在同一把锁上 → 整个写路径退化成单写者 → 吞吐 ≈ 1/fsync 延迟。
`everysec`/`no` 只在锁内做微秒级 `write()`，8 个 shard 保持并行。
「一条不丢」= fsync 之后才回包（durability-before-ack）。高吞吐 + 强持久的方向 = **组提交
（group commit）**：攒一批记录，一个后台线程对整批做一次 fsync，成功后一次性 ack 这批所有客户端。

### Q4. list 为什么不用 `std::deque`？
**要点**：`std::deque` 内部缓冲走 `std::allocator`（`::operator new`），**绕过 zmalloc 跟踪
分配器** → `used_memory()` 看不到它的内存 → `INFO` 低估、**maxmemory/淘汰判定基于偏小的数**，
真实内存超限却不触发淘汰。手写链表让每个节点和 value 都走 `zmalloc`，内存才计得准。

### Q5. 两个「漏网」
**要点**：① `max_connections` 在 `config.h` 解析了，但 `src/` 里**没有任何地方 enforce**
（accept 不检查连接数），配了不生效。② `volatile-lru` + 全库无 TTL key → `expires_` 空 →
采样不到候选 → 超限时返回 **OOM**，哪怕有一堆可用 allkeys-* 淘汰的 key。**这是配置语义，不是
bug**（与 Redis 一致）。

---

## 第 4 轮 · 通用系统 / C++

### Q1. ET vs LT
**要点**：LT 只要 fd 可读/可写就**一直通知**；ET 只在状态**跳变**时通知一次，之后不再通知。
所以 ET 下 `HandleRead` 必须读到 **EAGAIN**、`HandleWrite` 写到 **EAGAIN 或 buffer 清空**才停。
没读干净的后果是**连接饿死**（剩下的字节还在内核缓冲区，但没有新边沿就不会再被通知），**不是
「数据不完整」**。

### Q2. `shared_from_this` 与 `tie` 各防什么？
**要点**：`Connection::Send` 跨线程时 `shared_from_this()` 先抓强引用，防「回调异步执行时连接已
被自己线程关闭析构」→ 悬垂指针 / use-after-free。
`Channel::tie`(weak_ptr) 同样是防 use-after-free，但位置不同：分发时 `lock()` 一看宿主已死就
跳过回调。**不是**破循环引用（那是另一个 weak_ptr 用法）。

### Q3. 无锁的内存可见性 / 为什么 relaxed 够
**要点**：shard 存储无锁的正确性**不靠 atomic**，靠 EventLoop 交接本身：命令作为 functor 入队
（队列有 mutex）+ `eventfd` 唤醒 + 目标线程出队执行，这个跨线程过程天然建立 **happens-before**。
`key_count_`/`used_memory()`/`LruClock()` 是 atomic，因为它们是**跨线程读的统计量**（INFO 在 IO
线程汇总）。用 `relaxed` 够是因为没人依赖它们的精确顺序，只需保证单个 64 位读不撕裂。

### Q4. 路由用 `%`、bucket 用 `&`
**要点**：路由 `hash % shard_count`，因为 `shard_count` **不保证 2 的幂**。bucket 层 `hash &
sizemask`，因为 dict 的 size **恒为 2 的幂**、`sizemask = size-1` 低全 1，`& sizemask` ≡
`% size`。✅（首测已掌握）

### Q5. sds 为什么自己写、s[-1] 怎么定位
**要点**：`flags` 字节记录的是**头部的类型**（sdshdr8/16/32/64，即 len/alloc 各占几字节），
**不是**长度值本身；先读 `s[-1]` 确定类型，再按对应宽度读 len/alloc。`s[-1]=flags` 成立是因为
header 布局是 `{len; alloc; flags; buf[]}`，`sds` 指向 `buf`，前一个字节即 flags。
自己写 sds 的决定性理由：**sds 走 zmalloc，`used_memory()` 才数得到**；`std::string` 走
`std::allocator` 会绕过跟踪分配器（与 list 不用 deque 同一理由）。二进制安全、O(1) len 是
`std::string` 也有的，不算决定性理由。

---

## 第 5 轮 · 行为面

### BQ1. 最难的 bug
**答法**：STAR（现象 → 定位 → 根因 → 修复 → 验证），讲一个**真实的 debug**，别拿「没实现的
功能」充数。高概率候选：ET 没读干净导致连接饿死 / 跨线程 Send 忘 shared_from_this 的 UAF /
覆盖写 refcount 少减导致的泄漏 / AOF torn-tail 边界判定。挑一个能完整复述定位过程的。

### BQ2. 重来一次改哪个决策
**参考**：「我会一开始就认真设计多 key 命令的路由，而不是用 `first_key` 这种单 key 简化——
后来发现 MGET/MSET 几乎补不上，`DEL a b c` 还悄悄漏删。这是我最后悔的一个偷懒。」（要换成
你自己的真实判断，别背原话。）

### BQ3. 数字的诚实（13 万 QPS / 60%）
**模板**：「13 万是 pipelined 口径——每个客户端一次灌大量命令、批量回包，测的是服务端吞吐
上限，不是单条 round-trip；单条延迟受网络 RTT 主导。这数字能说明存储热路径无锁、瓶颈在内存
分配和哈希；不能说明真实延迟（那要看 P99，我们也是亚毫秒）和复杂命令表现。60% 的差距主要在
网络/解析层成熟度，不在存储。」

### BQ4. 砍掉一块
**要点**：说砍「AOF 的 `always` 档」比砍「分片」安全——always 只有强一致场景才用、成本高
（180 ops/s）收益窄；而**分片正是你简历的头号卖点**，砍它等于拆自己的台。若真要砍分片，必须
主动补一句代价：退回单线程、失去「无锁分片」这个亮点。

---

## 附录 · 高频陷阱清单（面试官顺手就戳的细节）

- **TTL 命令取整**：`TTL` 返回秒用 `(ms+500)/1000` **四舍五入**，Redis 是向下取整——小偏差，但
  被问到要能说清。
- **`max_connections` 不生效**：解析了但从没 enforce。
- **`StringToLongLong` 严格解析**：拒绝前导零（`0123` 不转 int）、`+`、`-0`、单独 `-`。
- **rehash 阈值**：扩容 load factor ≥ 1.0（Redis 还有 dict_can_resize 和 5.0 强制路径）；缩容
  < 1/8（Redis 是 0.1）。
- **lru 是 64 位 ms**：绕开了 Redis 24 位 lru 时钟约 194 天的回绕问题。
- **`SET` 固定 arity 3**：无 NX/XX/EX/PX/GET 选项；无 SETEX/SETNX/APPEND/STRLEN/MGET/MSET。
- **`DEL`/`EXISTS` 多 key 只作用第一个 key 的 shard**（见第 3 轮 Q1）。
- **`INCR` 溢出检查**：用 `__builtin_add_overflow`。
- **哈希函数**：MurmurHash64A（路由和 bucket 层同一个，只是用法不同）。
- **空占位头文件**：LFU、RDB（loader/writer/snapshot）、AOF rewrite、io_uring、zset、set、
  `Keyspace`（Week1 遗留）、`persistence_manager` 等——都是 stub/死代码，被问「支持 X 吗」时
  别答错。
- **错误文案**：协议错误返回的「invalid multibulk length」是照抄 Redis 的文案，但本项目是 RESP
  parser 不是 multibulk——小瑕疵。
- **`keyspace.cpp` 是死代码**：被 shard 取代，服务端不再引用。
