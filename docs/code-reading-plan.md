# mycache 全项目代码阅读方案

本文是一份**贯穿 Week1–5 的阅读地图**：把 ~50 个源文件按依赖顺序排好，逐文件给出「需要
知道的点」，并明确标出哪些文件是**脚手架/未接入的死文件**（可以直接跳过）。

- 已有单周深度复盘的，本文只给一句话 + 链接，不重复：`code-reading-week4.md`（AOF）、
  `code-reading-week5.md`（淘汰/监控/list）。
- 高层「是什么/为什么」看 `architecture.md`；本文讲「代码在哪、按什么顺序读、每段要懂什么」。
- 建议配合 `./build/mycache_tests`（78 个单测）一起看：读完一个模块就点开对应的 `tests/unit/*_test.cpp` 验证理解。

---

## 1. 一张图看懂整条链路

```
 redis-cli ──TCP──► 主线程 main_loop_
                       Acceptor (listen fd) ──accept4──► 新连接
                          │ round-robin 分给某个 IO 线程
                          ▼
                     IOReactor ×N  (各一线程 + EventLoop + epoll)
                       Connection::HandleRead 读 RESP
                          └─► Server::OnMessage → RespParser 解析
                              └─► Server::DispatchCommand
                                    ├─ 无 key(PING/ECHO/INFO)：IO 线程直接执行
                                    └─ 有 key：按 DictHash(key)%N 路由
                                          ▼
                                    ShardWorker ×N  (各一线程 + EventLoop + Shard)
                                      Shard: dict_(sds→Object*) + expires_(sds→到期ms)
                                         ├─ 写命令 → EvictOrOom → Set/HSET/LPUSH
                                         ├─ 写命令 → aof::LogCommand → AofWriter
                                         └─ 回复 → conn->Send（跨线程回 IO loop 刷出）

   AofWriter ── write/fsync ──► appendonly.aof   (启动时 AofRecovery::Replay 恢复到各 shard)
```

两个独立的线程体系：**IO 线程**管网络与协议，**ShardWorker 线程**管数据。它们之间靠
`EventLoop::RunInLoop`（把任务/回复排队到对方线程）来交接，靠 `Connection::shared_from_this`
保证交接期间连接不被析构。

---

## 2. 阅读顺序总表

> 由入口 → 底层 → 上层 → 装配。每行「文件 → 作用」，展开的「要点」见第 3 节。

### Phase 0 入口与配置
| 文件 | 作用 |
|------|------|
| `src/main.cpp` | 入口：读配置 → 初始化日志 → `Server::Start()` |
| `include/mycache/common/config.h` | 配置（header-only）：全部字段 + `ParseMemorySize` + `AofPath()` |
| `include/mycache/common/logger.h` | 日志（header-only，静态成员） |
| `include/mycache/common/time.h` | 时间：`NowMs()` 单调钟 / `UnixMs()` 墙钟 |

### Phase 1 协议层（自底向上，自包含）
| 文件 | 作用 |
|------|------|
| `protocol/resp_value.h` + `.cpp` | `RespValue`：一个值同时能表示命令与回复 |
| `protocol/resp_parser.h` + `.cpp` | RESP 增量解析，半包处理 |
| `protocol/resp_encoder.h` + `.cpp` | 把 `RespValue` 序列化成线上字节 |

### Phase 2 存储基础（自底向上）
| 文件 | 作用 |
|------|------|
| `storage/string/sds.h` + `.cpp` | 长度前缀字符串：O(1) 长度、二进制安全、预分配 |
| `storage/memory/allocator.h` + `.cpp` | `zmalloc` 跟踪分配器：`used_memory()` 的来源 |
| `storage/memory/memory_stats.h` + `.cpp` | 按编码统计字符串对象数量 |
| `storage/object/encoding.h` | `ObjType`/`ObjEncoding` 枚举（header-only） |
| `storage/object/object.h` + `.cpp` | `Object`（robj）：引用计数对象 + `lru` 字段 |
| `storage/object/string_object.h` + `.cpp` | 字符串对象：int vs string 两种编码 |

### Phase 3 哈希表 / 字典（核心数据结构）
| 文件 | 作用 |
|------|------|
| `storage/dict/hash_table.h` | `HashTable`（header-only）：桶数组 + size/sizemask/used |
| `storage/dict/rehash.h` + `.cpp` | 渐进式 rehash 策略参数 + 搬迁实现 |
| `storage/dict/dict.h` + `.cpp` | `Dict`：双表 + 链地址 + Find/Add/Replace/Delete |
| `storage/db/dict_types.h` | sds 键 + Object*/raw 值的 `DictType` 工厂（header-only） |

### Phase 4 分片存储
| 文件 | 作用 |
|------|------|
| `storage/db/shard.h` + `.cpp` | ★核心：`dict_`+`expires_` 双表、TTL、淘汰、原子计数 |
| `storage/db/keyspace.h` + `.cpp` | ⚠️ 旧 Week1 全局存储（已废弃，仅 benchmark 用） |

### Phase 5 集合类型
| 文件 | 作用 |
|------|------|
| `storage/collection/hash.h` + `.cpp` | Hash：嵌套 dict，placement new 构造 |
| `storage/collection/list.h` + `.cpp` | List：手写 zmalloc 双向链表 |

### Phase 6 淘汰与监控
| 文件 | 作用 |
|------|------|
| `eviction/eviction_policy.h` + `.cpp` | 5 种策略 + `EvictionConfig` |
| `eviction/lru.h` + `.cpp` | 缓存 LRU 时钟 |
| `eviction/sampler.h` + `.cpp` | 随机桶采样（rehash 感知） |
| `monitor/metrics.h` + `.cpp` | `Metrics` 单例（命中/淘汰/过期/命令/连接原子计数） |
| `monitor/info.h` + `.cpp` | `BuildInfo`：INFO 文本组装 |

### Phase 7 网络层（自上而下）
| 文件 | 作用 |
|------|------|
| `net/socket_ops.h` | socket 封装（header-only） |
| `net/buffer.h` + `.cpp` | 读写缓冲区（前置空间 + 读写下标） |
| `net/event_loop.h` + `.cpp` | `EventLoop` + `Channel`：epoll + eventfd + pending functor |
| `net/epoll_poller.h` + `.cpp` | epoll 封装（边沿触发） |
| `net/connection.h` + `.cpp` | `Connection`：读至 EAGAIN、写至 EAGAIN、优雅关闭 |
| `net/acceptor.h` + `.cpp` | 监听套接字 + accept 至 EAGAIN |
| `net/reactor.h` + `.cpp` | `IOReactor`：线程 + EventLoop + 连接表 |

### Phase 8 命令层
| 文件 | 作用 |
|------|------|
| `command/command.h` | `Command` 元数据（name/arity/first_key/handler） |
| `command/command_context.h` | `CommandContext` + `EvictOrOom` |
| `command/command_table.h` + `.cpp` | 注册 / 查找 / arity 校验 |
| `command/string_commands.cpp` | PING/SET/GET/DEL/ECHO/INCR/DECR/INCRBY/DECRBY |
| `command/generic_commands.cpp` | EXISTS/EXPIRE/PEXPIRE/TTL |
| `command/hash_commands.cpp` | HSET/HGET/HDEL/HGETALL |
| `command/list_commands.cpp` | LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN |
| `command/server_commands.cpp` | INFO/DBSIZE（无 key，IO 线程执行） |

### Phase 9 服务器装配（串起整条链路）
| 文件 | 作用 |
|------|------|
| `server/command_router.h` + `.cpp` | `DictHash(key)%N` 路由 + `ShardAt(i)` |
| `server/shard_worker.h` + `.cpp` | 每 shard 一线程 + timerfd 周期任务 |
| `server/server.h` + `.cpp` | ★装配：`Start`/`InitAof`/`OnMessage`/`DispatchCommand`/`Stop` |

### Phase 10 持久化（复用 Week4 深挖，此处只链接）
| 文件 | 作用 |
|------|------|
| `persistence/aof/aof_format.h` + `.cpp` | 记录格式：magic + `[len][crc32][RESP]` |
| `persistence/aof/aof_writer.h` + `.cpp` | 三种 fsync 策略 + 后台线程 |
| `persistence/aof/aof_recovery.h` + `.cpp` | 启动时重放 + 尾部修复 |

### Phase 11 测试（按子系统对照）
| 测试 | 对应 Phase |
|------|------|
| `tests/unit/resp_test.cpp` / `sds_test.cpp` / `object_test.cpp` | 1 / 2 |
| `tests/unit/dict_test.cpp` / `rehash_test.cpp` | 3 |
| `tests/unit/hash_test.cpp` / `ttl_test.cpp` / `router_test.cpp` | 4–5 |
| `tests/unit/lru_test.cpp` / `eviction_test.cpp` / `list_test.cpp` / `info_test.cpp` | 5–6 |
| `tests/unit/aof_test.cpp` | 10 |
| `tests/integration/*` / `tests/stress/*` / `benchmark/*` | 端到端 / 压力 / 直驱引擎 |

---

## 3. 逐文件要点

> 格式固定：**作用**（做什么）+ **需要知道的点**（为什么这样设计）+ **坑**（有则写）。
> 要点优先讲「取舍」，其次才是「做什么」。

### 3.0 入口与配置

#### `src/main.cpp`
**作用**：进程入口，十几行。

**需要知道的点**：
1. 只做三件事：`Config::Load` → `Logger::Init` → `Server::Start`。真正的逻辑全在 `Server` 里。
2. `Server::Start()` 是阻塞的（内部 `main_loop_.Loop()` 直到收到信号才退出），所以 `main` 直接 `return 0`。

#### `include/mycache/common/config.h`
**作用**：运行时配置（header-only，无 `.cpp`）。

**需要知道的点**：
1. **为什么 header-only**：`Config` 只被 `main.cpp` 和 `server.cpp` 使用，且全是很小的内联函数，不值得单独开一个编译单元。所有字段带默认值，`conf/mycache.conf` 只覆盖要改的项。
2. `ParseMemorySize`（`config.h:14`）：`k/m/g` 是 1000 进制、`kb/mb/gb` 是 1024 进制——刻意对齐 Redis 的 `memtoll` 约定。所以 `maxmemory 200mb` = `200*1024*1024`。
3. `AofPath()`（`config.h:69`）：`dir` + `appendfilename` 拼路径；空 `dir` 或 `"."` 就是当前目录。这是 Week4 加的。
4. `Load`（`config.h:74`）逐行 `key value` 解析，`#` 起头的行是注释——一个刻意简陋但够用的自研配置格式（不引入第三方库）。

#### `include/mycache/common/logger.h`
**作用**：极简线程安全日志（header-only，静态成员）。

**需要知道的点**：
1. 所有状态是 `inline static` 成员（C++17），所以能 header-only 且全局唯一。
2. 每次 `Log` 都拼一条 ≤2048 字节的行，然后 `mutex` 锁下 `fwrite + fflush`。**每条都 flush** 是为了崩溃时日志不丢（学习项目，重正确性轻性能）。
3. 用 `syscall(SYS_gettid)` 打线程号（不是 `getpid`），多线程排查时能看出日志来自 IO 线程还是 shard 线程。
4. `LOG_*` 宏自动塞 `__FILE__/__LINE__`，`LogLevel` 是数字，级别判断是「低于当前级别就不打」。

#### `include/mycache/common/time.h`
**作用**：两套时钟（header-only）。

**需要知道的点**：
1. `NowMs/NowUs` 用 `steady_clock`（**单调钟**，重启归零）→ 用于 uptime、限时 rehash 这类「时间差」。
2. `UnixMs`（`time.h:23`）用 `system_clock`（**墙钟**，Unix 纪元，重启不变）→ 用于 TTL 到期时间戳。
3. **为什么必须两套**：TTL 要写成 AOF 里的 `PEXPIREAT <绝对时间>`，重放时要求「重启后这个时间戳仍然有效」——只有墙钟满足。这是 Week4 的关键改动，细节看 `code-reading-week4.md` 的 Q5/A5。

### 3.1 协议层

#### `protocol/resp_value.h` + `resp_value.cpp`
**作用**：`RespValue`——一个 tagged union，既表示**解析出的命令**，也表示**回复**。

**需要知道的点**：
1. `type + str + integer + array` 四个字段共享一个结构：`kArray` 用 `array`，`kBulkString/kSimpleString/kError` 用 `str`，`kInteger` 用 `integer`。省一个类，代价是「哪个字段有效」靠 `type` 约定。
2. `kNull` 同时表示「null bulk string」（`$-1`）和「null array」（`*-1`）——编码器统一输出 `$-1`，够用即可。
3. 静态工厂 `Simple/Error/Integer/Bulk/NullBulk/Array` 只是图省事的构造器。

#### `protocol/resp_parser.h` + `resp_parser.cpp`
**作用**：增量式 RESP 解析。

**需要知道的点**：
1. **三态** `kOk/kIncomplete/kError`：`kIncomplete` 表示「字节还不够，别消费」，调用方（`server.cpp:198`）据此 `return` 等下一次读到更多字节再喂。这是处理 TCP 流式半包的核心机制。
2. **无状态**：`Parse` 每次都从头解析当前缓冲区，`*consumed` 只在 `kOk` 时推进。所以同一次 `OnMessage` 里可以 `while` 循环解析出多条命令（pipelining）。
3. `ReadLine` 用 `memchr` 找 `\n`，容忍单独的 `\n`（剥掉结尾可选 `\r`）——比 Redis 更宽松一点。
4. 上限对齐 Redis：bulk 长度 ≤512MB（`resp_parser.cpp:11`）、数组元素 ≤1M（`:12`），防止恶意报文撑爆内存。
5. 解析错误（非法类型字节、数字后面跟非 `\r\n`）返回 `kError`，上层直接回 `-ERR Protocol error` 并断开连接。

#### `protocol/resp_encoder.h` + `resp_encoder.cpp`
**作用**：`RespValue` → 线上字节。

**需要知道的点**：
1. 每种类型拼「类型前缀 + 内容 + CRLF」；bulk 是 `$<len>\r\n<bytes>\r\n`，null 固定 `$-1`。
2. 数组递归编码（`resp_encoder.cpp:38`），所以 `HGETALL/LRANGE` 的二维回复直接 `RespValue::Array` 就行。

### 3.2 存储基础

#### `storage/string/sds.h` + `sds.cpp`
**作用**：Simple Dynamic String——长度前缀、二进制安全的字符串。

**需要知道的点**：
1. `sds` 就是 `char*`，指向数据区；元数据藏在**数据前面的头部** `sdshdr8/16/32/64`（按长度选最小头部）。`s[-1]` 的 flags 字节反推头部类型，`s - sizeof(sdshdrN)` 反推头部地址。
2. **`__attribute__((packed))`**（`sds.h:38`）是为了让 flags 字节严格紧贴数据之前（即 `s[-1]`），对齐会破坏这个布局。
3. **O(1) 长度**：`sdslen` 读头部 `len`，不用 `strlen`；**二进制安全**：内容可含 `\0`，比较用 `sdscmp` 走 `memcmp`+长度，不是 `strcmp`。
4. `sdsMakeRoomFor`（`sds.cpp:48`）：扩容先 ×2，超过 1MB 后每次 +1MB——摊薄 realloc 次数。**头部类型升级**（8→16→32→64）时不能原地 realloc，要重新分配+搬数据（`:71`）。
5. **为什么不用 `std::string`**：核心诉求是「所有分配都走 `zmalloc` 被 `used_memory()` 统计」（见 allocator）。自研 sds 把分配控制在手，也和 Redis 语义对齐。

#### `storage/memory/allocator.h` + `allocator.cpp`
**作用**：带统计的分配器 `zmalloc/zcalloc/zrealloc/zfree` + `used_memory()`。

**需要知道的点**：
1. `used_memory()` 是**一个全局 `std::atomic<size_t>`**，所有引擎内存都过这里，于是它是整个淘汰与 INFO 的**唯一内存真相**。
2. **关键**：统计的是 `malloc_usable_size(p)`（`allocator.cpp:15`）——glibc 实际分配的大小，而不是你请求的大小。所以 `used_memory` 会略大于「各对象求和」，这也是 INFO 里 `used_memory` 与直觉对不上的原因之一。
3. `zrealloc` 只加/减**差值**（`allocator.cpp:45`），`zfree` 减 `usable_size`（`:51`）——两者必须对称，否则计数漂移。
4. 原子 + `memory_order_relaxed`：跨线程读计数不需要严格顺序，只要最终一致，relaxed 足够且几乎零开销（Week5 淘汰/INFO 都靠它）。

#### `storage/memory/memory_stats.h` + `memory_stats.cpp`
**作用**：按编码统计**字符串对象数量**（进程级单例）。

**需要知道的点**：
1. 与 `used_memory()`（字节数）互补：这个是**对象个数**，分 `int` / `string` 两桶。
2. `Incr/DecrString` 在 `CreateStringObject/CreateIntObject/TryObjectEncoding` 里同步维护——所以「字符串对象数」能反映 int 编码优化的效果（有多少字符串被编码成了整数）。

#### `storage/object/encoding.h`
**作用**：`ObjType`（string/list/hash/set/zset）与 `ObjEncoding`（int/string/ht/linkedlist/...）枚举。

**需要知道的点**：
1. 枚举里预留了一堆**未实现**的编码（`kListpack/kIntSet/kSkipList/kQuickList`）——这是「后续周次要做的」路线图，看到它们别以为已经实现了（对应第 4 节的死文件清单）。
2. `kLinkedList` 是 Week5 给 list 类型加的编码。

#### `storage/object/object.h` + `object.cpp`
**作用**：Redis 风格的引用计数对象 `Object`（robj）。

**需要知道的点**：
1. `Object` 五字段：`type/encoding/refcount/ptr/lru`。`ptr` 是 `void*`，指向的内容由 `encoding` 决定——string 指向 sds，hash 指向内嵌 Dict，list 指向 `List`。
2. **int 编码**：`long long` 直接**塞进 `ptr` 字段**（`object.h:12` 的 `ObjectPtrToLongLong`/反向），不额外分配。这是 Redis 的经典内存优化，也让 `INCR` 不必反复分配字符串。
3. `DecrRefCount`（`object.cpp:14`）归零后按 `type` 分派释放：string 释放 sds、hash 调 `ReleaseHashObject`、list 调 `ReleaseListObject`，最后 `zfree(this)`。**引用计数**让 dict 存值和临时变量可以共享同一个对象，不用担心重复释放。
4. `lru` 字段（`object.h:29`）在创建时初始化、每次访问刷新——Week5 淘汰的依据。

#### `storage/object/string_object.h` + `string_object.cpp`
**作用**：创建字符串对象 + int 编码转换 + 严格整数解析。

**需要知道的点**：
1. `CreateStringObject` / `CreateIntObject` 都记 `MemoryStats` 计数 + 初始化 `lru = LruClock()`。
2. `TryObjectEncoding`（`string_object.cpp:89`）：长度 ≤20 且能严格解析成整数时，释放 sds、原地改成 int 编码。**为什么 20**：`long long` 十进制最多 20 字节。`SET` 和 `INCR` 都会走它。
3. `StringToLongLong`（`string_object.cpp:12`）是**严格版**：拒绝前导零（`"0123"`）、`'+'`、空白、空串、单独 `"-"`。**为什么严格**：对齐 Redis 的 `string2ll`——`GET` 一个 `"0123"` 应该是字符串而不是被误判成整数；`INCR` 对 `"abc"` 要报「not an integer」而不是悄悄转成 0。
4. 溢出处理：解析时用 `ULLONG_MAX` 上限判断（`:39`/`:42`），负数边界特判 `LLONG_MIN`（`:50`）。

### 3.3 哈希表 / 字典

#### `storage/dict/hash_table.h`
**作用**：单张哈希表的骨架（header-only，`Dict` 持有两张）。

**需要知道的点**：
1. `table`（桶数组，每个桶是 `DictEntry*` 链）、`size`（桶数，**2 的幂**）、`sizemask = size-1`（用于 `idx = hash & sizemask`）、`used`（条目数）。
2. 两表设计：`ht[0]` 服务请求、`ht[1]` 是 rehash 目标——这是渐进式 rehash 的硬件基础。

#### `storage/dict/rehash.h` + `rehash.cpp`
**作用**：渐进式 rehash 的策略参数 + 搬迁实现。

**需要知道的点**：
1. **为什么渐进**：大表一次性搬迁会卡住所有请求（O(N) 停顿）；渐进式把搬迁摊到每次写操作里「顺带搬一桶」，单次延迟有界。
2. `rehashidx_` 是状态机：`-1` = 未 rehash；`>=0` = 下一个待搬桶下标。`Rehashing()` 就是 `rehashidx_ != -1`。
3. `Rehash(n)`（`rehash.cpp:54`）搬 n 个**非空**桶，空桶跳过但受 `empty_visits` 上限约束（`kRehashEmptyVisitFactor=10`）——避免稀疏表上一次调用扫太多空桶。搬完一桶整条链后 `ht[0].used` 归零就 free + swap + `rehashidx_=-1`（`:82`）。
4. 扩容/收缩：`ExpandIfNeeded`（`:38`）在 `used >= size`（负载因子 1）时翻倍；`ShrinkIfNeeded`（`:45`）在 `used*8 < size` 且 `size>初始值`（负载因子 <1/8）时收缩。负载因子 1 和 1/8 都是 Redis 的常数。

#### `storage/dict/dict.h` + `dict.cpp`
**作用**：`Dict`——链地址 + 双表 + 渐进 rehash 的完整字典。

**需要知道的点**：
1. `DictType`（`dict.h:20`）让 dict 和「键/值怎么哈希、比较、复制、销毁」解耦。注意 `equal` 是**布尔**（Redis 的 dict 返回 -1/0/1，这里改了语义，`dict.h:19` 注释明说）。
2. `DictHash`（`dict.cpp:9`）是 **MurmurHash64A**——它同时被 dict 分桶**和 shard 路由**（`command_router.cpp:24`）使用。所以「同一个 key 在哪个 shard、在哪个桶」用的是同一个哈希。
3. `AddRaw`（`dict.cpp:89`）写路径三件事：先 `RehashStep` 推进 rehash → 再 `ExpandIfNeeded` 触发扩容 → 最后**若 rehash 中写 `ht[1]`，否则写 `ht[0]`**（`:109`）。这是保证「写都落到新表、rehash 能收敛」的关键。
4. `Find`（`dict.cpp:143`）是**只读**的，不推进 rehash；rehash 中要**查两张表**（`:146` 的 `for t in 0..1`，靠 `Rehashing()` 决定是否查第二张）。→ 这就是 Week1「读用共享锁、不写」能成立的原因。分片后新增非 const 的 `FindAndRehash`（紧随 `Find` 之后）：先 `RehashStep` 再查两张表，供单线程 Shard 读路径推进 rehash（对齐 Redis `dictFind`）。
5. `Replace`（`dict.cpp:129`）对已存在键「先 `FreeVal` 旧值再 `SetVal` 新值」——因为 val 是 `Object*`（引用计数），必须让旧值减引用，否则泄漏。
6. 键默认 `key_dup = sdsdup`（`SetKey`，`:67`）：**dict 存键的副本**，插入后调用方的 sds 要自己 `sdsfree`。这个「谁拥有键」的约定贯穿 shard.cpp（到处都是 `sdsnewlen`…`sdsfree` 成对出现）。

#### `storage/db/dict_types.h`
**作用**：把「sds 键 + 各种值」的 `DictType` 集中定义（header-only）。

**需要知道的点**：
1. `SdsObjectDictType`（`dict_types.h:39`）：sds 键 + `Object*` 值，`val_dup = ObjectDup`（incref）、`val_destructor = ObjectFree`（decref）——主键空间和 hash 字段表共用。
2. `SdsRawDictType`（`:51`）：sds 键 + **裸 `void*` 值**，`val_dup/val_destructor` 都是空——专门给 `expires_` 用，因为它的值是 `int64_t` 到期时间戳强转的 `void*`，由 `Shard` 自己管理，dict 不该去 free。
3. 抽到独立头文件是为了避免 `Keyspace/Shard/hash` 三处重复定义同一套回调。

### 3.4 分片存储

#### `storage/db/shard.h` + `shard.cpp` ★核心
**作用**：单个 shard 的完整存储——`dict_`（sds→Object*）+ `expires_`（sds→到期 ms），以及 TTL、淘汰、各类型命令。

**需要知道的点**：
1. **无锁**：`Shard` 只被它归属的那个 `ShardWorker` 线程访问（`shard.h:19` 注释）。这是 Week3 用「分片」替换 Week1「全局锁 Keyspace」的全部动机——省掉每请求一次锁。
2. **惰性删除**：`LookupValue`（`shard.cpp:119`）先 `ExpireIfNeeded` 查 `expires_`，到期就删并返回 null；没到期才查 `dict_` 并刷新 `lru`。所有读路径（GET/HGET/EXISTS/LPOP…）都过这里，所以「过期 key 读不到」是**在访问时**才生效的。
3. `Set`（`shard.cpp:142`）顺序：`TryObjectEncoding` → **先 `expires_.Delete(k)` 清 TTL** → 再 `dict_.Replace(k, o)` → 最后 `o->DecrRefCount()`。要点：SET 覆盖一个带 TTL 的 key 必须清掉它的 TTL（Redis 语义）；`DecrRefCount` 释放本函数持有的引用，dict 内部已 incref。
4. **原子计数** `key_count_/expire_count_`（`shard.h:86`）：`INFO`/`DBSIZE` 在 IO 线程汇总，不能跨线程直接读 `dict_.Size()`（数据竞争），所以在每个写路径 `fetch_add/sub` 维护，跨线程只 `load`。这是 Week5 的线程安全设计。
5. 淘汰在 `EvictIfNeeded/EvictOne/SampleCandidates/PickVictim`——完整讲解与「按 shard 均分的等价性」看 `code-reading-week5.md`（Q3/A3、Q4/A4）。
6. `ExpireAt`（`shard.cpp:395`）把到期时间戳 `int64_t` 强转 `void*` 存进 `expires_`；`ActiveExpireCycle`（`:432`）是**主动**过期（随机采 20 个桶），补足惰性删除「没人访问就不删」的漏洞。

#### `storage/db/keyspace.h` + `keyspace.cpp`
**作用**：⚠️ 旧 Week1 的**全局**字符串存储（`shared_mutex` 保护）。

**需要知道的点**：
1. 这是「分片化之前」的样子：一个 `Dict` + 一把 `shared_mutex`，读共享锁、写独占锁。
2. **读不加写锁的原因**：`Dict::Find` 只读、不推进 rehash，所以共享锁下读安全；rehash 只在写路径推进，写路径持独占锁。
3. 现在**运行中的服务器不用它**（`server.cpp` 用的是 `Shard`），只有 `benchmark/million_keys.cpp` 直驱它做内存压测。读它的价值是**对比**：理解「全局锁 → 分片无锁」这个演进。
4. 注意它自己又内联了一份 `SdsObjectDictType()`（`keyspace.cpp:41`）——比 `dict_types.h` 早写，后来 `Shard` 复用同一套逻辑时抽到了 `dict_types.h`。看到重复别奇怪。

### 3.5 集合类型

#### `storage/collection/hash.h` + `hash.cpp`
**作用**：hash 类型——`Object(kHash, kHt)`，`ptr` 指向一个内嵌 `Dict`（field sds → value Object*）。

**需要知道的点**：
1. `CreateHashObject`（`hash.cpp:12`）用 **placement new** 在 `zmalloc` 出来的裸内存上构造 `Dict`：`new (d) Dict(SdsObjectDictType())`。原因：`zmalloc` 是 `malloc`，不跑构造函数；而 `Dict` 有非平凡析构（`Clear`），必须显式构造。
2. `ReleaseHashObject`（`:28`）对称地**显式调析构** `d->~Dict()` 再 `zfree`。这一对「placement new / 显式析构」是手写分配器下的惯用套路，也是内存泄漏/双重释放的高发点。
3. 外层 `Shard::HSet` 复用同一套「dict 存值用引用计数」的约定（`dict_types.h` 的 `SdsObjectDictType`）。

#### `storage/collection/list.h` + `list.cpp`
**作用**：list 类型——手写 `zmalloc` 双向链表（`ListNode`/`List`）。

**需要知道的点**：
1. **为什么不用 `std::deque`**：`std::deque` 内部用标准分配器 `operator new`，会**绕过** `zmalloc` 的统计，列表占的内存就从 `used_memory()` 里「消失」了——淘汰时水位对不上。手写链表让每个节点/value 都过 `zmalloc`。完整取舍看 `code-reading-week5.md` Q7/A7。
2. **那个真实的 bug**：`ListNode` 写了 `prev=nullptr; next=nullptr;` 默认成员初始化器，但 `zmalloc` 是 `malloc` **不执行**它，`NewNode` 必须显式置空（`list.cpp:10` 的注释）。第一版漏了 `next`，尾节点 `next` 是垃圾指针，`ReleaseListObject` 沿 `next` 遍历时崩（ASan 抓的 SEGV）。细节看 `code-reading-week5.md` Q6/A6。
3. `ListPop*` 把节点的 sds `value` **归还给调用方**（`n->value = nullptr` 再 `zfree(n)`，`list.cpp:79`）——所有权转移的约定，调用方（`shard.cpp` 的 LPop/RPop）负责 `sdsfree(v)`。

### 3.6 淘汰与监控

#### `eviction/eviction_policy.h` + `eviction_policy.cpp`
**作用**：`EvictionPolicy` 枚举（5 种）+ `EvictionConfig` + 解析。

**需要知道的点**：
1. 5 种策略对应 Redis `maxmemory-policy`：`noeviction` / `allkeys-lru` / `allkeys-random` / `volatile-lru` / `volatile-random`。`volatile-*` 只淘汰**带 TTL** 的 key。
2. `EvictionConfig` 是**按 shard 复制**的同一份全局配置（`server.cpp:41` 的 `MakeEvictionConfig`），`maxmemory` 是全局字节数，每个 shard 各自对照它。

#### `eviction/lru.h` + `lru.cpp`
**作用**：近似 LRU 的缓存时钟。

**需要知道的点**：
1. `LruClock`（`lru.cpp:13`）读全局原子；`c==0` 时自初始化——保证单测不依赖后台线程也能拿到非零时钟。`RefreshLruClock` 由 shard 的 100ms timerfd 调用。
2. **为什么缓存时钟**：`GET` 每次命中都要在 `LookupValue` 里 `o->lru = LruClock()`，热路径上每次读系统时钟有成本；缓存成原子后只是一次 `load`。细节看 `code-reading-week5.md` Q1/A1。

#### `eviction/sampler.h` + `sampler.cpp`
**作用**：`SampleDictEntries`——随机挑桶采样。

**需要知道的点**：
1. **rehash 感知**：`total = size0 + size1`（`sampler.cpp:13`），随机下标落在前半段查 `ht[0]`、后半段查 `ht[1]`——否则 rehash 期间会漏掉已迁到 `ht[1]` 的 key。
2. `max_steps = count*10`（`:19`）限制尝试次数，避免稀疏表上空转。这也是「采样可能采不够 key」的边界来源。细节看 `code-reading-week5.md` Q2/A2、Q11/A11。

#### `monitor/metrics.h` + `metrics.cpp`
**作用**：进程级统计单例（命中/未命中/淘汰/过期/命令/连接），全部原子。

**需要知道的点**：
1. 全是 `std::atomic` + `relaxed`，跨线程累加无锁。`connected_clients` 是「当前在线」，连接建立/关闭时 `Incr/Decr`；其余是「累计」。
2. `MarkStart`（`metrics.cpp:12`）用 `compare_exchange_strong` 实现**幂等**——多次调用只记第一次。`UptimeSeconds` 基于 `NowMs()`（单调钟）。

#### `monitor/info.h` + `info.cpp`
**作用**：组装 `INFO` 输出文本。

**需要知道的点**：
1. `BuildInfo`（`info.cpp:60`）拼 server/clients/memory/stats/keyspace 各节。memory 里 `used_memory` 来自 `used_memory()`，RSS 来自 `ResidentMemoryBytes()`（读 `/proc/self/statm`，`:40`）。
2. **碎片率虚高**：`mem_fragmentation_ratio = RSS/used_memory`，数据少时 `used` 只有几百字节而 RSS 是 MB 级（线程栈/libc 都在 RSS 里），比值能到几千。口径解释看 `code-reading-week5.md` Q12/A12。
3. keyspace 计数是**跨线程汇总**各 shard 的原子 `Size()/ExpireCount()`（`info.cpp:72`）——这正是 `shard` 里原子计数存在的意义。

### 3.7 网络层

#### `net/socket_ops.h`
**作用**：socket 系统调用的 header-only 封装。

**需要知道的点**：
1. `CreateListenSocket`（`socket_ops.h:34`）：`SOCK_NONBLOCK | SOCK_CLOEXEC` + `SO_REUSEADDR` + `bind` + `listen`，一步到位。
2. `SetTcpNoDelay` 关 Nagle——交互式响应要低延迟，`redis-benchmark` 那种小包场景不开它延迟会很难看。

#### `net/buffer.h` + `buffer.cpp`
**作用**：带前置空间的连续字节缓冲区（读/写都用它）。

**需要知道的点**：
1. 三指针模型（muduo 风格）：`read_index_` / `write_index_`，中间是 `readable`，前面是 `prependable`（预留 8 字节）、后面是 `writable`。
2. `ensureWritable`（`buffer.cpp:9`）：空间不够时**优先 `memmove` 紧凑**（把可读段挪回开头）而不是直接 `resize`——省一次分配。
3. `readFd/writeFd` 直接对 fd 做一次 read/write 并维护下标。

#### `net/event_loop.h` + `event_loop.cpp`
**作用**：`EventLoop`（线程 + epoll + 回调队列）与 `Channel`（fd + 关注事件 + 回调）。

**需要知道的点**：
1. **eventfd wakeup**：跨线程想唤醒 epoll（例如 shard 线程把回复排到 IO 线程）时，往 `wakeup_fd_` 写 1 字节，epoll 立即返回去跑 pending functor（`Wakeup`→`HandleWakeup`，`event_loop.cpp:75/81`）。
2. `RunInLoop` vs `QueueInLoop`（`:59/67`）：在本线程就直接执行，否则入队+wakeup。这是所有跨线程交接的基础设施（路由、写回、建连都靠它）。
3. `Channel::Tie`（`event_loop.h:46`）持一个 `weak_ptr` 指向拥有者（Connection/Acceptor），`HandleEvent` 先 `tie_.lock()`（`event_loop.cpp:142`）——**防 use-after-free**：事件分发期间拥有者被别的线程析构时，回调就不执行了。
4. `DoPendingFunctors` 用 `swap` 把队列换出来再执行（`:87`），避免执行过程中新入队的 functor 被锁卡住。

#### `net/epoll_poller.h` + `epoll_poller.cpp`
**作用**：epoll 的薄封装，**一律边沿触发（ET）**。

**需要知道的点**：
1. `WithET`（`epoll_poller.cpp:9`）总是 `| EPOLLET`，且只在关注 `EPOLLIN` 时才加 `EPOLLRDHUP`（半关闭检测）——否则完全禁用的通道会被 RDHUP 骚扰。
2. **ET 的后果**：就绪后必须**读/写直到 EAGAIN**，否则剩下的数据不会再触发事件。这解释了为什么 `Connection::HandleRead/HandleWrite` 都是 `while(true)` 循环。
3. `Poll`（`:41`）对 `EINTR` 重试一次。

#### `net/connection.h` + `connection.cpp`
**作用**：一条 TCP 连接（IO 反应器持有）。

**需要知道的点**：
1. `HandleRead`（`connection.cpp:64`）ET 读至 `EAGAIN` 后，把整个输入缓冲交给 `message_cb_`（`Server::OnMessage` 会解析出**多条**命令）。
2. **跨线程写回**：`Send`（`:29`）检测「不在 loop 线程」就 `RunInLoop` 排队，并捕获 `shared_from_this()` 保证连接在写回调执行前不被析构——这是 shard 线程回复能安全落回 IO 线程的关键。
3. **优雅关闭**：`Shutdown`（`:55`）若还有未发完的输出，先置 `closing_` 待写空再关（`HandleWrite` 末尾 `if (closing_) HandleClose()`）。
4. `HandleClose`（`:101`）先 `channel_->Remove()`（EPOLL_CTL_DEL）再让 close 回调走，**避免 fd 复用时与残留的 epoll 注册项冲突**。

#### `net/acceptor.h` + `acceptor.cpp`
**作用**：主线程上的监听套接字。

**需要知道的点**：
1. `accept4(... SOCK_NONBLOCK | SOCK_CLOEXEC)` 一次拿非阻塞 fd，`while` 接受直到 `EAGAIN`（`acceptor.cpp:33`）。
2. 对 `EMFILE/ENFILE`（fd 耗尽）单独告警并 break（`:42`），避免死循环。

#### `net/reactor.h` + `reactor.cpp`
**作用**：`IOReactor` = 一个线程 + 一个 EventLoop + 一张连接表。

**需要知道的点**：
1. `AddConnection`（`reactor.cpp:20`）把建连调度到 loop 线程执行（`RunInLoop`），因为 `Connection`/`Channel` 都必须在它自己的线程上创建。
2. close 回调里 `erase + DecrConnected`（`:25`）——`connection_count_`/`connected_clients` 都是原子，供 INFO 读。

### 3.8 命令层

#### `command/command.h`
**作用**：`Command` 元数据 + 五个 `Register*Commands` 声明。

**需要知道的点**：
1. `arity` 语义（`command.h:16`）：正值 = 参数个数**恰好**等于（含命令名）；负值 = **至少** `|arity|` 个。所以 `set` 是 `3`（SET k v 恰好 3 个），`del` 是 `-2`（至少 2 个）。
2. `first_key` 是路由 key 在 args 里的下标，`0` = 无 key（IO 线程直接执行）。这是「有 key 走 shard、无 key 就地跑」的开关。

#### `command/command_context.h`
**作用**：`CommandContext`——handler 执行时能拿到的所有东西 + 回复辅助 + `EvictOrOom`。

**需要知道的点**：
1. `db`（`Shard*`）对无 key 命令是 `nullptr`；`aof/config/router` 各是「有就带上」的可选指针。
2. `Reply*` 系列（`command_context.h:27`）统一走 `conn->Send(EncodeResp(...))`——所以 handler 只关心「回什么」，不关心「怎么发」。
3. `EvictOrOom`（`:36`）是所有「可能增大内存」的写命令（SET/HSET/INCR/LPUSH/RPUSH）写**之前**调用的统一入口：超限且无法淘汰就回 OOM。为什么在写之前——看 `code-reading-week5.md` Q4/A4。

#### `command/command_table.h` + `command_table.cpp`
**作用**：大小写不敏感的命令注册表 + arity 校验。

**需要知道的点**：
1. 存 `std::unordered_map<string, Command>`；大小写不敏感是在**上层** `DispatchCommand` 里先把命令名 `tolower` 再查（`server.cpp:220`），表本身只存小写。
2. `CheckArity`（`command_table.cpp:16`）实现正/负 arity 语义，错误串直接拼成 Redis 风格的 `ERR wrong number of arguments ...`。

#### `command/string_commands.cpp`
**作用**：PING/SET/GET/DEL/ECHO/INCR 家族。

**需要知道的点**：
1. `SetCommand`（`:28`）顺序：`EvictOrOom` → `db->Set` → `aof::LogCommand("SET", ...)` → 回复。**先改内存再记日志**（同 week4 约定）。
2. `IncrByCommand`（`:63`）：read-modify-write，`__builtin_add_overflow` 判溢出；**AOF 记成 `SET key <结果>` 而不是 `INCR`**——为了重放幂等。取舍看 `code-reading-week5.md` Q10/A10。
3. `DelCommand`（`:46`）只在 `removed>0` 时记 AOF——删不存在的 key 不该产生日志。
4. `ECHO` 是 `redis-cli --pipe` 的同步哨兵（注释 `:55`），所以它必须精确回显。

#### `command/generic_commands.cpp`
**作用**：EXISTS/EXPIRE/PEXPIRE/TTL。

**需要知道的点**：
1. `ExpireCommand`（`:40`）把相对秒数算成**一次** `when = UnixMs() + s*1000`，内存和 AOF 存的都是这同一个值——重放结果确定（Week4 Q6/A6）。
2. `TtlCommand`（`:65`）返回**秒**且 `(ms+500)/1000` 向最近秒舍入（Redis TTL 语义）；`ms<0` 时透传 -2（不存在）/ -1（永不过期）。

#### `command/hash_commands.cpp` / `command/list_commands.cpp`
**作用**：hash 与 list 的命令。

**需要知道的点**：
1. `CheckHashType`/`CheckListType` 先查 `Type(key)`，非目标类型回 `WRONGTYPE ...`（Redis 的错误语义），避免「对 string key 做 HSET」时静默出错。
2. `HGetAll`/`LRange` 用 `RespValue::Array` 拼多元素回复。
3. `LPop/RPop` 是**破坏性写**，记成 `LPOP/RPOP`（重放时逐次弹出，效果一致）；`LPush/RPush` 记成**参数一致**的 `LPUSH/RPUSH`（重放按同顺序重建列表）。

#### `command/server_commands.cpp`
**作用**：INFO / DBSIZE（都是无 key，IO 线程执行）。

**需要知道的点**：
1. `InfoCommand`/`DbSizeCommand` 依赖 `ctx.config`/`ctx.router`（IO 线程直接注入，见 `server.cpp:253`）——所以它们能聚合**全部** shard，不绑定某个 shard。
2. 无 key 命令 `db == nullptr`，handler 里绝不能碰 `ctx.db`。

### 3.9 服务器装配

#### `server/command_router.h` + `command_router.cpp`
**作用**：key → shard 的映射与任务派发。

**需要知道的点**：
1. `ShardIndex = DictHash(key) % N`（`command_router.cpp:24`）——和 dict 分桶同一个哈希，保证分布均匀。
2. `Route`（`:28`）→ `ShardWorker::Submit` → `RunInLoop`，把 handler 排队到目标 shard 线程。**同一个 key 的命令因此在同一个线程按 FIFO 顺序执行**（无并发、无需锁）。

#### `server/shard_worker.h` + `shard_worker.cpp`
**作用**：一个 shard worker = `Shard` + `EventLoop` + 线程 + timerfd 周期任务。

**需要知道的点**：
1. `OnTimer`（`shard_worker.cpp:64`）每 100ms 做三件事：`RefreshLruClock()` → `ActiveExpireCycle()` → `EvictIfNeeded()`。第三件是**后台兜底**淘汰——即使之后没有新写入，超限也会被压回。
2. **为什么 timerfd 而不是 sleep 线程**：timerfd 是个 fd，能塞进 epoll 跟其他事件统一分发，不用额外线程、不用加锁。
3. `Submit`（`:43`）就是 `RunInLoop([s, fn] { fn(s); })`——shard 的所有访问都收敛到这一个线程。

#### `server/server.h` + `server.cpp` ★装配
**作用**：把上面一切装配起来。

**需要知道的点**：
1. **启动顺序**（`Start`，`server.cpp:70`）是刻意编排的：忽略 SIGPIPE → `Metrics::MarkStart` → `RefreshLruClock` → **`InitAof`（必须在线程启动前）** → 启动 IO reactor → 启动 shard worker → 信号处理 → acceptor → `main_loop_.Loop()`。`InitAof` 在前是因为恢复要**单线程直接写 shard**，worker 起来后就有数据竞争了（Week4 Q4/A4）。
2. `OnMessage`（`:185`）是「一次读入的字节里可能有多条命令」的循环：`while(buf->readable()>0)` 解析 → `DispatchCommand`。开头跳过多余的 CR/LF（`redis-cli --pipe` 会带）。
3. `DispatchCommand`（`:209`）：小写命令名 → 查表 → arity 校验 → `IncrCommands` → 按 `first_key` 决定**路由到 shard**（带 aof/config/router 进 context）还是 **IO 线程直接执行**（`db=nullptr`）。
4. `Stop`（`:120`）优雅关闭：停 acceptor → 停 IO reactor → 停 shard worker → 关 AOF（flush+fsync）→ quit 主循环。顺序反过来拆。

### 3.10 持久化（详见 `code-reading-week4.md`）

`aof_format`（记录格式 `[len][crc32][RESP]`）→ `aof_writer`（三种 fsync 策略 + 后台线程）→
`aof_recovery`（启动重放 + 三条损坏判断 + 尾部截断）。这一整块 Week4 已经逐段讲过，读代码时
配合 `code-reading-week4.md` 第 4 节的逐段讲解和第 5 节自查题即可，这里不重复。

---

## 4. 死文件 / 脚手架清单（可直接跳过）

这些文件**未接入运行中的服务器**（有的是预留实现、有的只剩壳）。判断依据：没有被 `src/*.cpp`
直接 include，也不在 `server.cpp` 的启动/分发路径上。读它们容易「以为在跑、其实没跑」，先记住
它们是死代码。

| 文件 | 为什么是死代码 |
|------|------|
| `eviction/lfu.h` | LFU 未实现，只留枚举占位 |
| `storage/collection/zset.h` | zset 未实现（无 .cpp） |
| `expire/{expire_manager,timer_wheel,ttl_index}.h` | 过期只走了 `Shard::expires_` 的惰性/主动删除，这些是未启用的抽象 |
| `persistence/rdb/{rdb_loader,rdb_writer,snapshot}.h` | RDB 持久化未实现 |
| `persistence/aof/aof_rewrite.h` | AOF 重写未实现 |
| `persistence/persistence_manager.h` + `.cpp` | 包装层未接入（server 直接用 AofWriter/AofRecovery） |
| `io/{io_engine,posix_io,uring_io}.h`（含 `.cpp`） | io 抽象未用；server 直接用 `epoll_poller` |
| `monitor/slowlog.h` | 慢日志未实现 |
| `storage/db/database.h` | 未实现（空壳） |
| `storage/memory/memory_pool.h` | 内存池未接入（分配器直接 malloc） |
| `command/command_executor.h` + `.cpp` | `.cpp` 为空文件，未使用 |
| `server/lifecycle.h` | 未接入（Start/Stop 写在 server.cpp） |
| `common/{error,status}.h`、`protocol/protocol_error.h` | 错误统一走 `RespValue::Error`，这套 Status 类型未用 |
| `benchmark/benchmark_runner.h` | 未使用（benchmark 各自写在 main 里） |

> 注：`CMakeLists.txt` 用 `GLOB_RECURSE src/*.cpp`，所以上表里**有 `.cpp` 的死文件
> （`io/*.cpp`、`persistence_manager.cpp`、`command_executor.cpp`、`keyspace.cpp`）依然会被
> 编译进 `mycache_core` 库**，只是不参与运行**。`keyspace.cpp` 例外——它被 benchmark 直驱，
> 属于「活着但只服务压测」的代码。

---

## 5. 怎么用这份方案

1. **按 Phase 0 → 11 顺序读**，同一 Phase 内按表格从上到下。每个 Phase 读完，跑对应的
   `tests/unit/*_test.cpp`（见总表对照）看行为是否符合你的理解。
2. **遇到「为什么」卡住**：先看本文件「需要知道的点」，再看对应周次的深度文档；还不懂再去
   读代码注释（本项目注释质量很高，关键取舍都写在注释里）。
3. **画一条请求的完整旅程**（最好的自测）：`redis-cli set k v` 从 `Acceptor` 到 `Shard::Set`
   到 AOF 到回复，把每个函数在脑里串一遍；再串一条 `get k`（命中/未命中/过期三种情况）。
4. **验证链路是否真通了**：`./scripts/build.sh` 后 `./scripts/acceptance.sh`（端到端）、
   `./scripts/eviction_test.sh`（淘汰）、`./scripts/crash_test.sh`（AOF 崩溃恢复）。

读完这 11 个 Phase，你就能说出「一个 key 从网络字节到落盘，中间经过了哪些线程、哪些结构、
哪些取舍」，这也就是整个项目想让你学到的东西。
