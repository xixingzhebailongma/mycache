# 架构(第 1 周)

## 线程模型:主 reactor + IO reactor

```
                  ┌───────────────┐
 client ──TCP────►│  main thread  │  epoll (ET) on listen fd, accept4()
                  │  main_loop_   │
                  └──────┬────────┘
                         │ round-robin hand-off (RunInLoop)
       ┌─────────────────┼─────────────────┐
       ▼                 ▼                 ▼
 ┌───────────┐     ┌───────────┐     ┌───────────┐
 │ IO reactor│     │ IO reactor│     │ IO reactor│   each: own thread + epoll (ET)
 │ conns...  │     │ conns...  │     │ conns...  │
 └───────────┘     └───────────┘     └───────────┘
```

- `EventLoop` —— 一个 epoll 实例 + 一个 `eventfd` 唤醒通道 + 一个待执行的
  functor 队列。`RunInLoop`/`QueueInLoop` 提供线程安全的交接。
- `Channel` —— fd + 关注的事件 + 回调;弱引用 `tie` 在事件分发期间维持所属对象
  存活(避免 close 后的 use-after-free)。
- `Connection` —— 输入/输出 `Buffer`,ET 读至 EAGAIN 与写至 EAGAIN,flush 之后
  优雅关闭。
- `Acceptor` —— 非阻塞的监听 fd;一直 accept 直到 `EAGAIN`。
- `IOReactor` —— 在工作线程中运行一个 `EventLoop` 并拥有其连接。

## 单条命令的数据流

1. `Connection::HandleRead` 排空 socket,然后触发 `MessageCallback`。
2. `Server::OnMessage` 从输入 buffer 中解析一条或多条 RESP 命令。
3. `Server::DispatchCommand` 将命令名小写化,在 `CommandTable` 中查找,检查
   arity,并以 `CommandContext` 调用处理函数。
4. 处理函数读写共享的 `Keyspace`,并通过 `conn->Send(...)` 回复,后者把数据追加
   到输出 buffer 并启用 `EPOLLOUT`。

## 存储

- `sds` —— 带长度前缀、二进制安全的字符串;`len`/`alloc` 存放在紧邻 buffer 前面
  的头部(`sdshdr8/16/32/64`)中,长度为 O(1),追加为均摊 O(1)。
- `Object` —— 带引用计数的 Redis 风格对象。字符串值使用两种编码之一:
  `int`(long long 内联在 `ptr` 中)或 `string`(单独分配的 sds)。
- `Dict` —— 拉链法哈希表,持有两张表(`ht[0]`、`ht[1]`)用于渐进式 rehash。
  扩容/缩容时分配 `ht[1]` 并设置 `rehashidx`;此后每次写操作迁移一个桶,直到
  `ht[0]` 排空、两张表交换。
- `Keyspace` —— 由 `std::shared_mutex` 保护、被所有 IO reactor 共享的封装。
  读者取共享锁且为纯读(不 rehash);写者取独占锁,且每次操作推进一个桶的 rehash。
- `allocator` / `MemoryStats` —— 一个追踪分配器,为 `used_memory()` 提供数据,
  另加按编码分类的对象计数。

## 关键设计决策(第 1 周范围)

- 使用一个全局 `Keyspace`(加锁)而非每分片存储;分片放到后续周实现。
- 回复被缓冲并批量 flush,这正是流水线(pipelining)廉价的原因。
- 信号处理使用 `eventfd` + 一个 channel,使 `Stop()` 在 loop 线程上运行;
  `SIGPIPE` 被忽略。
- rehash 只在写路径上推进(共享锁读永远不会修改 dict),这使 `shared_mutex` 模型
  在并发下保持健全。

## 第 3 周:分片执行 + hash + TTL

在命令层,单一的全局 `Keyspace` 被一组分片取代。每个 `Shard` 拥有独立的
`dict`(sds -> Object*)和一个 `expires` 字典(sds -> 绝对过期毫秒数),并且只会被
它自己的 `ShardWorker` 线程访问,因此无需加锁。分片让 `shared_mutex` 消失,于是
读路径也改用 `Dict::FindAndRehash` 推进 rehash(对齐 Redis `dictFind`)。

```
                    ┌───────────────┐
  client ──TCP─────►│  main thread  │ accept4 + hand-off (round-robin)
                    └──────┬────────┘
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                  ▼
  ┌───────────┐      ┌───────────┐      ┌───────────┐
  │ IO reactor│      │ IO reactor│      │ IO reactor│  parse RESP, route
  └─────┬─────┘      └─────┬─────┘      └─────┬─────┘
        │   key-hash: DictHash(key) % N        │
        ▼                  ▼                  ▼
  ┌───────────┐      ┌───────────┐      ┌───────────┐
  │ ShardWorker│ ... │ ShardWorker│      │ ShardWorker│  own thread + EventLoop
  └─────┬─────┘      └─────┬─────┘      └─────┬─────┘
        └──── reply via Connection::Send (hops back to IO loop)
```

- `CommandRouter` 用 `DictHash(key) % shard_count` 将 `key -> shard` 映射,并把命令
  作为 functor 提交到所属 `ShardWorker` 的 `EventLoop`。因此同一 key 的命令在单个
  线程上按 FIFO 顺序执行。
- 回复回写路径(`Worker -> IO Reactor`)是 `Connection::Send`,它检测到跨线程调用后,
  通过 `shared_from_this()` 重新入队到该连接的 IO loop,以在 flush 之前保持连接存活。
- 无 key 命令(`PING`、`ECHO`)在 IO 线程上内联执行。
- Hash 值是类型为 `kHash` 的 `Object`,其 `ptr` 是一个嵌套的字段(sds)-> 值(Object*)
  字典。
- TTL 采用惰性删除(每次查找时检查)加上一个简化的主动过期:每个分片的 `timerfd`
  每 100 ms 触发一次,对 `expires` 字典随机采样若干桶,移除已过期 key。
- 本周已知的裁剪:跨分片的 `MGET`/`MSET` 和 `MULTI`;多 key 的 `DEL`/`EXISTS`
  按第一个 key 路由,只影响那个分片。

## 第 4 周:AOF 持久化 + 崩溃恢复

分片写路径被扩展为追加式命令日志,使数据集在 `kill -9` 后依然存活。完整设计见
`docs/persistence.md`;要点如下:

```
 command ──► ShardWorker ──► mutate Shard ──► AofWriter::Append(record) ──► appendonly.aof
                                                          ▲
                              write() every append        │
                              fsync: always/everysec/no   │
                                                          │
   startup ──► AofRecovery::Replay ──► validate + replay + repair partial tail
```

- `AofWriter` —— 一个共享的、由 mutex 保护的写者。每次 append 都会 `write()` 每条
  记录(因此 `kill -9` 不丢任何东西);fsync 策略(`always`/`everysec`/`no`)只控制
  页缓存刷盘的频率。`everysec` 会运行一个后台 fsync 线程。
- `AofRecovery` —— 在任何分片工作线程启动之前于启动阶段运行,因此可以通过
  `CommandRouter::ShardAt` 直接写入每个 `Shard`。每条记录是一个
  `[len][crc32][RESP command]` 帧;撕裂的尾部(`kill -9` 造成的部分写)会被检测到并
  截断。
- 写处理函数(`SET`/`DEL`/`HSET`/`HDEL`/`EXPIRE`)通过 `CommandContext::aof` 记录日志。
  TTL 命令被翻译成绝对时间 `PEXPIREAT`,因此 `Shard` 的过期现在使用墙上时钟
  (`UnixMs()`)。

## 第 5 周:内存治理 + metrics + list

### 淘汰(每分片、基于采样)

第 3 周的分片设计在此得到回报:每个 `Shard` 恰好由一个 `ShardWorker` 线程拥有,
所以淘汰是普通的每线程操作,热路径上**没有存储锁**。导致内存增长的写操作
(`SET`/`HSET`/`INCR`/`LPUSH`/`RPUSH`)的流程如下:

```
command ──► ShardWorker ──► EvictIfNeeded() ──► (over limit?)
                                  │ no: proceed to mutate
                                  │ yes + noeviction: reply OOM
                                  │ yes + lru/random: EvictOne() loop until under
```

- **单一事实来源** —— 追踪分配器的 `used_memory()`。每分片配额
  (`maxmemory/shard_count`)对比每分片用量(`used_memory()/shard_count`)可化简为
  *同一个*全局比较,因为 key 被均匀哈希分布,所以每个分片只需对照自己的 key 检查
  全局水位。其代价(热分片可能在别的分片还冷时过度淘汰自己的 key)记录在
  `docs/eviction.md`。
- **近似 LRU** —— `Object` 携带一个 `uint64_t lru`(Redis 的 robj 模型),在创建时
  设置、每次查找时刷新。一个缓存的 `LruClock()`(原子变量,由 100 ms 分片定时器
  刷新,对应 Redis 的 `serverCron`)使热路径免于每次访问都做时钟 syscall。淘汰时
  对两张哈希表随机游走采样 `maxmemory_samples` 个 key,删除其中 `lru` 最小的那个。
- **策略** —— `noeviction`(默认)、`allkeys-lru`、`allkeys-random`、`volatile-lru`、
  `volatile-random`。volatile 变体采样 `expires` 字典,因此只会淘汰带 TTL 的 key。

### Metrics + INFO

一个进程级的 `Metrics` 单例(原子变量)追踪命中/未命中、被淘汰/被过期 key、命令、
连接和运行时长。每个 `Shard` 还维护原子的 `key_count_`/`expire_count_`(在
`Set/Del/DeleteKey/ExpireAt/HSet/HDel` 中维护),使 `INFO` 能在 IO 线程上无数据竞争地
汇总跨分片的 key/expire 计数。`INFO`(`monitor/info.cpp`)组装 server/clients/memory/
stats/keyspace 各节;`mem_fragmentation_ratio` 为 `RSS / used_memory`(RSS 来自
`/proc/self/statm`)。

### List 类型 + INCR

`INCR`/`DECR`/`INCRBY`/`DECRBY` 复用整数字符串编码,并把绝对结果以 `SET` 写入 AOF
日志以实现确定性重放。list 类型是一个普通的双向链表(`storage/collection/list.cpp`
中的 `ListNode`/`List`),其中每个节点和值都经过 `zmalloc`,因此 list 内存能被
`used_memory()` 正确计数 —— 不像 `std::deque` 会绕过追踪分配器。
