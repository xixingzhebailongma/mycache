# 面试精讲：按目录顺序的代码走读 Q&A

> 配合 `docs/code-reading-plan.md` 的阅读顺序使用。每个目录一节，头文件与对应 `.cpp` 合并讲解。
> 格式：**Q = 中文问题，A = 中文参考答案**（要点式，标注 `文件:行号` 便于回看代码）。
> 标注「⚠️ 未实现/stub」的文件面试时不要当成已实现来讲。

## 目录（阅读顺序）

1. [command/](#1-command命令注册与执行上下文) — Command 结构 · CommandContext · CommandTable · multi_key 扇出 · 各命令 handler
2. [common/](#2-common配置日志状态时间) — Config · Logger · Status · Error · Time
3. [eviction/](#3-eviction内存淘汰) — 淘汰策略 · LRU 时钟 · 采样器
4. expire/ —— ⚠️ 全部空壳，跳过（TTL 实现在 storage/db 的 `Shard`，见 §11）
5. [io/](#5-io文件-io-引擎) — IOEngine 抽象 · posix · io_uring
6. [monitor/](#6-monitor监控指标) — Metrics · BuildInfo
7. [net/](#7-net网络-reactor) — EventLoop · Channel · Connection · Buffer · Acceptor · EpollPoller
8. [persistence/](#8-persistence持久化) — PersistenceManager · SnapshotGate · AOF(writer/rewrite/recovery/format) · RDB(loader/writer)
9. [protocol/](#9-protocolresp-协议) — RespValue · RespParser · RespEncoder
10. [server/](#10-server编排路由) — Server · CommandRouter · ShardWorker
11. [storage/](#11-storage存储核心) — dict · sds · object · shard · memory · collection
12. [入口与构建](#12-入口与构建) — main.cpp · CMakeLists.txt

---

## 1. command/（命令注册与执行上下文）

### 1.1 `command.h` — `Command` 结构与路由元数据

**Q1：`Command` 里的 `arity` 如何表达命令参数个数？正负号分别代表什么？**

**A1：** 正值 = 参数个数必须**恰好**等于该值（**含命令名本身**）；负值 = **至少** `|arity|` 个参数。例：`set` 是 `3`（`SET key value` 恰好 3 个），`ping` 是 `-1`（可带可不带参数），`mset` 是 `-3`（至少 3 个，即至少 1 对 key/value）。见 `include/mycache/command/command.h:14-17`。

**Q2：路由元数据 `first_key` / `last_key` / `key_step` 各是什么？`first_key == 0` 意味着什么？**

**A2：**
- `first_key`：路由 key 在 args（含命令名）中的下标；`0` 表示**无 key**，命令在调用线程（IO 线程）直接执行，不路由到 shard。
- `last_key`：`<0` 表示 key 从 `first_key` 一直取到 args 末尾；`>0` 表示最后一个 key 的下标；`==0` 表示单 key / 无 key（沿用 first_key 语义）。
- `key_step`：相邻 key 的下标差。`MSET` 是 key/value 交替所以为 `2`。

见 `command.h:19-27`。

### 1.2 `command_context.h` — `CommandContext` 与 `EvictOrOom`

**Q3：`CommandContext` 里塞了哪些指针？为什么无 key 命令（PING/INFO）的 `db` 是 `nullptr`？**

**A3：** 字段：`conn`（连接）、`db`（所属 shard 的存储指针）、`aof`（AOF writer，非空时写命令会追加日志）、`config`（全局配置）、`router`（命令路由器）、`pm`（持久化协调器）。无 key 命令在 IO 线程直接执行、不属于任何 shard，所以 `db = nullptr`，它们靠 `config`/`router`/`pm` 触达全局状态。见 `command_context.h:21-27`。

**Q4：`EvictOrOom` 的作用？为什么每个写命令前都要调它？**

**A4：** 在可能增大内存的写命令前调用：若内存超 `maxmemory` 且无法淘汰，回复 OOM 错误并返回 `false` 阻止写入。它把「maxmemory 淘汰」这一横切关注点统一收敛到写路径入口，避免每个 handler 各自重复判断。见 `command_context.h:38-44`。

### 1.3 `command_table.h/.cpp` — 注册表与 arity 校验

**Q5：命令名大小写不敏感是怎么实现的？在表里还是在外层？**

**A5：** `CommandTable` 本身只是 `std::unordered_map<std::string, Command>`，`Find` 是**精确匹配**（`src/command/command_table.cpp:11-14`）。大小写不敏感实际由调用方 `Server::DispatchCommand` 实现：查找前把命令名 `std::tolower` 转小写（`src/server/server.cpp:216-220`）。这是「把规范化放在入口、表保持简单」的做法——注释里说的「大小写不敏感」是整体效果，不是表本身。

**Q6：`CheckArity` 的校验规则与 Redis 报错格式？**

**A6：** `arity>=0` 时要求 `argc == arity`；`arity<0` 时要求 `argc >= -arity`。失败时 `snprintf` 拼出 `ERR wrong number of arguments for '<name>' command`。见 `command_table.cpp:16-30`。

### 1.4 `multi_key.h/.cpp` — 多 key 命令的 scatter-gather（重要）

**Q7：`GroupKeysByShard` 如何把多 key 命令的 key 分到各 shard？为什么记录「原下标」而不是拷贝 key 字符串？**

**A7：** 对每个 key 计算 `router.ShardIndex(key)`（= `DictHash(key) % shard_count`），把落到同一 shard 的 key 聚成一组；组内只记录 key 在原 keys 数组里的**下标** `indices`（按原顺序）。用下标而非拷贝，是为了保留 `MGET` 这类「按位置回填结果」的语义，且省一次字符串拷贝。见 `src/command/multi_key.cpp:10-26`。

**Q8：`FanOut` 的「finish 恰好执行一次」如何保证？为什么用 `acq_rel` 内存序？**

**A8：** `FanOutState` 持有一个 `std::atomic<size_t> remaining` 初始化为 group 数；每个 shard 子任务完成后 `Complete()` 里 `fetch_sub(1, memory_order_acq_rel)`，返回值为 1 的那个线程（**最后一个完成者**）调用 `on_last`。`acq_rel`：release 侧发布本任务写入的聚合结果，最后一个的 acquire 侧读到所有先前的 release，从而 `on_last` 能看到**全部**部分结果，建立正确的 happens-before。见 `multi_key.cpp:31-60`。

**Q9：为什么多 key 写命令（如 MSET）不保证跨 shard 原子性？**

**A9：** 各 shard 在自己的线程/锁内独立执行，扇出后没有跨 shard 的事务协调。以 `MSET` 为例，逐 shard 写，某个 shard 触发 OOM 就置 `oom` 标志跳过，而其他 shard 已经写入——所以 OOM 时可能**部分成功**。这是用「按 shard 分区并行 + 最终一致性」换吞吐的典型取舍；Redis 单线程不存在此问题。见 `src/command/string_commands.cpp:124-145`。

### 1.5 `string_commands.cpp` — SET/GET/DEL/MGET/MSET/INCR 族

**Q10：为什么 DEL/MGET/MSET 要扇出到各 shard，而 SET/GET 只在单个 shard 上执行？**

**A10：** 单 key 命令的 key 经哈希唯一确定一个 shard，路由到该 shard 线程执行即可；多 key 命令的 key 可能散落在不同 shard，需按 shard 分组扇出、最后汇总。注册时 `SET/GET` 用 `first_key=1` 单 key 路由，`DEL/MGET/MSET` 用 `last_key=-1` 标记多 key。见 `string_commands.cpp:208-220`。

**Q11：为什么 INCR/DECR 在 AOF 里记成 `SET key <结果>` 而不是记 INCR 增量？**

**A11：** INCR 是读-改-写，结果取决于当前值；记成 `SET key <绝对结果>` 后，AOF 重放时结果是确定的绝对值，不依赖增量语义，也无需重放端实现 INCR 的读改写，幂等自包含。见 `string_commands.cpp:173-176`。

**Q12：MGET 的结果为什么能按原顺序回填？**

**A12：** 预先分配 `results` 数组（大小为 key 数，默认 `kNull`），每个 shard 的 work 用 key 的原下标 `i` 直接写 `(*results)[i]`。不同 key 下标不同、各写各的槽位互不冲突（无锁），最后 finish 时把整个数组编码回包。见 `string_commands.cpp:81-101`。

**Q13：SET 处理里为什么先 `EvictOrOom` 再 `Set`？**

**A13：** SET 会分配新值、可能增大内存，必须在写前检查 maxmemory；超限且无法淘汰则直接回 OOM，避免内存无限增长。见 `string_commands.cpp:31-36`。

### 1.6 `generic_commands.cpp` — EXISTS/EXPIRE/PEXPIRE/TTL

**Q14：EXPIRE/PEXPIRE 为什么要记成 PEXPIREAT 而不是 EXPIRE？**

**A14：** `EXPIRE` 是相对秒数，`PEXPIREAT` 是绝对毫秒时间戳。记绝对时间后，AOF 重放结果与「服务器开机时间 / 重放耗时」无关，TTL 语义稳定。见 `generic_commands.cpp:60-83`。

**Q15：TTL 返回秒数时的取整规则？为什么是 `(ms + 500) / 1000`？**

**A15：** Redis 的 TTL 返回秒、向**最近秒舍入**。`(ms + 500) / 1000` 是整数除法实现四舍五入；`ms < 0`（`-1` 无 key / `-2` 无 TTL）时原样返回负值。见 `generic_commands.cpp:85-89`。

**Q16：EXISTS 对重复 key 怎么计数？**

**A16：** 按 Redis 语义每个出现的 key 都计数（即使重复）。因为同一个 key 一定哈希到同一个 shard，逐 key 判断即可保留重复语义。见 `generic_commands.cpp:42-51`。

### 1.7 `hash_commands.cpp` + `list_commands.cpp` — 类型检查与 AOF

**Q17：为什么 HSet 前要 `CheckHashType` 检查 key 现有类型？**

**A17：** 避免把已存 string 的 key 当 hash 用导致数据损坏；Redis 语义是「key 已存在且类型不符时回 WRONGTYPE」。`CheckHashType` 在类型为 `hash` 或 `none` 时放行，否则回 `WRONGTYPE Operation against a key holding the wrong kind of value`。见 `src/command/hash_commands.cpp:15-20`。

**Q18：LPOP/RPOP 为什么也记 AOF？弹出后 key 可能被删，重放如何保证一致？**

**A18：** 弹出是破坏性写，重放时逐次 LPOP 效果一致（列表状态逐步还原）；若弹出后列表为空，Shard 会删除该 key，重放同样会走到空删除。见 `src/command/list_commands.cpp:54-75`。

### 1.8 `server_commands.cpp` — INFO/DBSIZE/BGREWRITEAOF/SAVE/BGSAVE

**Q19：DBSIZE 是怎么算的？为什么遍历所有 shard 累加？**

**A19：** 数据被哈希分片到 N 个 shard，总 key 数 = 各 shard `Size()` 之和，所以要 `for i in [0, shard_count): n += router->ShardAt(i)->Size()`。见 `server_commands.cpp:25-36`。

**Q20：INFO/DBSIZE/BGREWRITEAOF 这些命令的 `first_key=0`，它们在哪执行？**

**A20：** `first_key=0` 表示无 key，直接在 IO 线程执行、不路由到 shard，用 `ctx.config`/`ctx.router`/`ctx.pm` 访问全局状态。见 `server_commands.cpp:76-81`。

## 2. common/（配置·日志·状态·时间）

### 2.1 `config.h` — 配置解析与 `ParseMemorySize`

**Q1：`ParseMemorySize` 里 `k/m/g` 和 `kb/mb/gb` 有什么区别？为什么要有这种区分？**

**A1：** `k/m/g` 是 **1000 进制**（k=1000、m=1000²、g=1000³），`kb/mb/gb` 是 **1024 进制**（1024、1024²、1024³），与 Redis 的 `memtoll` 约定一致。这样 `1k` = 1000 字节、`1kb` = 1024 字节，兼顾人类习惯的十进制和存储的二进制。解析末尾还有溢出检查 `n > INT64_MAX / mult` 防止整数溢出。见 `include/mycache/common/config.h:14-44`。

**Q2：`Config::Load` 怎么解析配置文件？支持注释吗？**

**A2：** 逐行 `getline`，先 `find('#')` 把 `#` 之后的注释截掉，再用 `istringstream` 按空白拆成 `key value`，调 `Apply(key, value)` 赋值。`#` 是注释符；行内不足两个 token 的行直接跳过。见 `config.h:86-100`。

**Q3：与内存/持久化相关的关键配置项及其默认值？**

**A3：** `maxmemory=0`（不限制）、`maxmemory_policy=noeviction`、`maxmemory_samples=5`、`appendonly=false`、`appendfsync=everysec`、`dbfilename=dump.rdb`、`io_engine=posix`。见 `config.h:57-72`。

**Q4：`AofPath()` / `RdbPath()` 怎么拼完整路径？`dir` 为空或 `.` 时如何处理？**

**A4：** `dir` 为空或 `.` 时直接用文件名（当前目录）；否则 `dir + "/" + filename`。见 `config.h:75-84`。

### 2.2 `logger.h/.cpp` — 异步日志

**Q5：Logger 为什么要异步落盘？FATAL 为什么例外？**

**A5：** 非 FATAL 日志只把格式化好的字节 append 到 `pending_` 缓冲，由后台线程每 ~5ms 批量 `fwrite + fflush`，避免逐行 fflush 造成一行一个 `write(2)` 系统调用，降低日志对主路径的延迟影响。FATAL 必须同步直写，保证 `abort`/return 前消息一定落盘、不被后台线程吞掉。见 `logger.h:14-22`、`src/common/logger.cpp:79-85`。

**Q6：`fork()` 后子进程为什么不能打日志？**

**A6：** `fork` 后子进程只剩调用线程存活；Logger 持有后台线程和 `mutex_`，若子进程调用 `LOG_*`，`mutex_` 可能处于「被另一个已不存在的线程持有」的不一致状态，会死锁或丢日志。所以 AOF rewrite / RDB bgsave 子进程只做无分配操作并 `_exit`，不打日志。见 `logger.h:20-22`。

**Q7：`Shutdown` 如何保证幂等且不丢日志？**

**A7：** 用 `running_.exchange(false)` 做一次性判定（幂等）；先 `join` 后台线程，再持锁把 `pending_` 残留写空并 `fflush`，最后若非 stderr 则 `fclose`。见 `logger.cpp:39-53`。

**Q8：每条日志包含哪些字段？线程号怎么拿到？**

**A8：** 格式 `MM-DD HH:MM:SS [tid] [LEVEL] file:line message`；时间用 `localtime_r`（线程安全），线程号用 `syscall(SYS_gettid)` 取真实内核 TID（而非 pthread 的缓存 id）。见 `logger.cpp:61-68`。

### 2.3 `status.h` / `error.h` / `time.h`

**Q9：`Status` 相比直接返回 `ErrorCode` 有什么好处？**

**A9：** 同时携带一个错误码 + 可选消息，`ToString()` 能拼成 `ErrorName: message`，比裸枚举多了可读的上下文；`ok()` 判断简洁。见 `include/mycache/common/status.h:10-39`。

**Q10：为什么有 `steady_clock`（`NowMs`/`NowUs`）和 `system_clock`（`UnixMs`）两套时钟？分别用在哪？**

**A10：** `steady_clock` 单调递增、不受系统时间调整影响，用于测耗时/定时（如 LRU 时钟、ActiveExpireCycle 间隔）；`system_clock` 是墙钟（Unix 纪元），进程重启后仍一致，用于 TTL 到期时间戳和写 AOF 的 `PEXPIREAT`，重放时才可靠。`UnixMs` 等价于 Redis 的 `mstime()`。见 `include/mycache/common/time.h:8-27`。

## 3. eviction/（内存淘汰）

> ⚠️ `lfu.h` 是空壳（注释说明 LFU 未实现），面试时不要当成已实现。

### 3.1 `eviction_policy.h/.cpp` — 策略与共享整数开关

**Q1：项目支持哪几种淘汰策略？与 Redis 的 `maxmemory-policy` 如何对应？**

**A1：** 五种：`noeviction`（超限拒绝写、回 OOM）、`allkeys-lru`（所有 key 采样淘汰最久未访问）、`allkeys-random`（所有 key 随机淘汰）、`volatile-lru`（仅在有 TTL 的 key 里采样淘汰最久未访问）、`volatile-random`（仅在有 TTL 的 key 里随机淘汰）。见 `include/mycache/eviction/eviction_policy.h:15-21`。

**Q2：`EvictionConfig` 里的 `maxmemory` 是全局还是 per-shard 的？**

**A2：** 头文件注释明确 `maxmemory` 是**全局**内存上限（字节，0 = 不限制）；但每个 shard 只在自己的写路径上、从自己的 key 里淘汰，不直接访问别的 shard。把全局上限拆成 per-shard 水线的逻辑在 `Shard` 里（见 §11）。见 `eviction_policy.h:23-29`。

**Q3：为什么 LRU 策略下 `AllowSharedIntegers` 返回 false？运行期为什么不能切换？**

**A3：** LRU 淘汰依赖每个对象**独立的 lru 时间戳**；共享整数对象只有一份 lru，被多个 key 共享会破坏「谁最久未访问」的采样。所以 LRU 策略关闭共享整数，random/noeviction 允许（对齐 Redis 的 `MAXMEMORY_FLAG_NO_SHARED_INTEGERS`）。且开关随策略在**启动期一次性确定**：一旦共享对象已写入 dict，value 就是共享指针，切回 LRU 也无法让已存对象重新拥有独立 lru（除非 O(N) 全量重建）。见 `src/eviction/eviction_policy.cpp:5-13`、`eviction_policy.h:31-38`。

### 3.2 `lru.h/.cpp` — 近似 LRU 时钟

**Q4：LRU 时钟为什么用全局原子缓存，而不是每次 key 访问读系统时钟？多久刷新一次？**

**A4：** 为省去每次 key 访问都读系统时钟的开销，用一个全局 `std::atomic<uint64_t>` 缓存当前毫秒，由后台定时器（shard worker 的 100ms 周期）刷新——与 Redis `serverCron` 每 100ms 刷新 `server.lruclock` 一致。见 `lru.h:7-16`。

**Q5：`LruClock` 里 `c == 0` 的懒初始化是干什么的？**

**A5：** 首次调用若时钟还没被定时器初始化（值为 0），立即读一次墙钟写回，保证单测里不依赖后台线程也能得到非零时钟。见 `src/eviction/lru.cpp:13-20`。

### 3.3 `sampler.h/.cpp` — 随机采样

**Q6：`SampleDictEntries` 怎么采样？rehash 期间怎么处理？**

**A6：** 随机挑桶、沿链收集，最多取 `count` 个；rehash 期间两张表（ht[0]、ht[1]）都参与——先算总桶数 `size0 + size1`，`idx` 落在哪张表就采哪张。类似 Redis 的 `dictGetSomeKeys`。见 `src/eviction/sampler.cpp:12-32`。

**Q7：为什么有 `max_steps = count * 10` 的上限？采样结果严格均匀吗？**

**A7：** 防止在稀疏表里随机撞空桶空转太久；采样是**近似**的，不必严格均匀，够近似 LRU/random 淘汰用。表很小或连续命中空桶时返回条目数可能少于 `count`。见 `sampler.h:12-17`、`sampler.cpp:18-31`。

**Q8：用「近似 LRU」而非「精确 LRU」的取舍是什么？**

**A8：** 精确 LRU 需要维护全局有序链表/堆，每次访问都要更新（高开销、多线程下还要加锁）；近似 LRU 只需随机采样 N 个 key（`maxmemory_samples`，默认 5）挑其中最久未访问的淘汰，O(1) 无锁、开销恒定。Redis 本身也用近似 LRU（采样 + 淘汰最老），精度略降但吞吐大幅提升。见 `eviction_policy.h:23-29`、`sampler.h`。

## 5. io/（文件 I/O 引擎）

### 5.1 `io_engine.h/.cpp` — 抽象与工厂

**Q1：为什么要把文件 I/O 抽象成 `IOEngine` 接口？接口只覆盖哪些操作？**

**A1：** 让 AOF/RDB 的持久化路径可以通过配置 `io_engine` 无缝切换 `posix` / `io_uring` 两种引擎。接口只覆盖热路径：`Open/Write/Pread/Fsync/Ftruncate/Close`；`rename` 不是 io_uring 热点，仍由调用方用普通 syscall 完成。见 `include/mycache/io/io_engine.h:10-27`。

**Q2：`Write`/`Pread` 的「写满/读满」语义怎么保证？**

**A2：** 内部循环处理 EINTR（信号中断重试）与部分读写，直到写满 `len` 或遇到错误/EOF。`Write` 返回写入总字节数（失败 -1），`Pread` 返回实际读到字节数（EOF 提前结束）。见 `io_engine.h:13-14`、`src/io/posix_io.cpp:14-41`。

**Q3：工厂 `CreateIOEngine` 的回退逻辑？**

**A3：** `name == "io_uring"` 时：若编译进 liburing 且 ring 初始化成功返回 `UringIO`，否则告警回退 `PosixIO`；未编译 liburing 也告警回退。其余一律 `PosixIO`。见 `src/io/io_engine.cpp:11-25`。

### 5.2 `uring_io.h/.cpp` — io_uring 引擎

**Q4：为什么 `UringIO` 只把 `write`/`fsync` 走 io_uring，其余操作复用 `PosixIO`？**

**A4：** AOF/RDB 的热路径主要在顺序写 + fsync；`open/pread/ftruncate/close` 不是热点，复用 posix 减少复杂度。这是「把 io_uring 用在刀刃上」的务实取舍。见 `uring_io.h:12-14`。

**Q5：`UringIO::Write` 里 `offset = -1`（`static_cast<__u64>(-1)`）是什么意思？**

**A5：** 表示使用文件当前位置（等价于普通 `write` 不指定 offset），由内核 write 路径维护并推进文件偏移，`O_APPEND` 语义也由内核保证。见 `src/io/uring_io.cpp:22-23`。

**Q6：当前 `UringIO` 是同步还是异步？注释里说的「真正收益」是什么？**

**A6：** 当前是**同步** submit + `wait_cqe`（提交一个 SQE 就等一个 CQE），主要用于演示 io_uring 的提交/完成模型和压测对比；真正的收益来自将来把多个 SQE **批提交**（异步），减少提交/完成往返的开销。见 `uring_io.h:12-14`。

**Q7：⚠️ io_uring 引擎会不会用到 socket（网络）？**

**A7：** 不会。`IOEngine` 只做**文件 I/O**（AOF/RDB），网络始终是 epoll（见 §7 net）。这是常见易错点：面试要分清「io_uring 文件 I/O」和「epoll 网络」两个不同层次。

## 6. monitor/（监控·指标）

> ⚠️ `slowlog.h` 是空壳，未实现。

### 6.1 `metrics.h/.cpp` — 进程级统计

**Q1：`Metrics` 为什么用原子计数器 + `relaxed` 内存序？连接数为什么有增也有减？**

**A1：** 计数会被多个线程（各 IO 线程、shard 线程）同时累加，用 `std::atomic` 保证无数据竞争；这些计数不参与任何同步逻辑（只是统计），所以用 `relaxed` 即可、无需昂贵的内存屏障。`connected_clients` 是「当前连接数」，新连接 `IncrConnected`、断开 `DecrConnected`（有增有减）；其余是单调累计（只增）。见 `include/mycache/monitor/metrics.h:13-20`。

**Q2：`MarkStart` 为什么用 `compare_exchange` 而不是直接赋值？`UptimeSeconds` 用的是什么钟？**

**A2：** `MarkStart` 要幂等（只记录第一次启动时刻），用 CAS 保证多线程同时调用也只写一次。`UptimeSeconds` 基于 `NowMs()`（steady_clock 单调钟）算差值，不受系统时间调整影响。见 `src/monitor/metrics.cpp:12-21`。

### 6.2 `info.h/.cpp` — INFO 输出

**Q3：INFO 里的内存碎片率怎么算？RSS 怎么拿？**

**A3：** `mem_fragmentation_ratio = RSS / used_memory`；`used` 由 zmalloc 统计（见 §11），RSS 读 `/proc/self/statm` 的 resident 字段 × 页大小。比值大于 1 说明存在内存碎片。见 `src/monitor/info.cpp:40-63`。

**Q4：`keyspace_hits_ratio` 怎么算？**

**A4：** `hits / (hits + misses)`；`lookups == 0` 时返回 0.0 防除零。见 `info.cpp:65-68`。

**Q5：INFO 输出用什么换行符？为什么？**

**A5：** 用 `\r\n`（CRLF），与 Redis 的 INFO 输出一致，`redis-cli` 能正确解析。见 `info.cpp:85-114`。

**Q6：为什么 INFO 的 keyspace 节和 DBSIZE 都要遍历所有 shard 累加？**

**A6：** 数据哈希分片到 N 个 shard，任何「全库统计」（总 key 数、总 expires 数）都必须遍历所有 shard 累加，单个 shard 只持有自己那份。见 `info.cpp:70-76`。

## 7. net/（网络 Reactor）

### 7.1 `socket_ops.h` — socket 工具

**Q1：监听 socket 创建用了哪些 flag？为什么 `SO_REUSEADDR`？**

**A1：** `socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)` —— 非阻塞 + 子进程不继承 fd；`SO_REUSEADDR` 允许快速重启时在 TIME_WAIT 状态下重新绑定端口。见 `include/mycache/net/socket_ops.h:34-64`。

**Q2：`TCP_NODELAY` 是什么？为什么 accept 后要设置？**

**A2：** 关闭 Nagle 算法，避免小包（如单条 RESP 回复）被延迟合并发送，降低延迟——对 KV 服务器低延迟至关重要。见 `src/net/acceptor.cpp:49`。

### 7.2 `buffer.h/.cpp` — 连续缓冲区

**Q3：`Buffer` 为什么要留前置空间 `kCheapPrepend`？读写用哪两个下标维护？**

**A3：** 前置空间允许在 buffer 头部廉价地插入协议头/长度字段而不搬数据；`read_index_`/`write_index_` 把缓冲区分成「已读 / 未读 / 可写」三段，`readable() = write - read`、`prependable() = read`。见 `buffer.h:16-25`。

**Q4：`ensureWritable` 里「先 memmove 回收前置空间、再 resize」的策略？**

**A4：** 空间不够时，若 `前置空间 + 可写 >= 需求`，就 `memmove` 把未读数据搬到 `kCheapPrepend` 处、回收已读掉的空间；否则才真正 `resize` 扩容——避免无谓扩容。见 `src/net/buffer.cpp:9-19`。

### 7.3 `epoll_poller.h/.cpp` — epoll 封装

**Q5：`WithET` 为什么只在关注 `EPOLLIN` 时才加 `EPOLLRDHUP`？**

**A5：** `EPOLLET` 边沿触发避免水平触发下的忙轮询；`EPOLLRDHUP` 表示对端关闭写半连接，只在关注输入时请求，这样完全禁用读的通道保持安静、不收无关事件。见 `src/net/epoll_poller.cpp:9-14`。

### 7.4 `event_loop.h/.cpp` — Channel 与 EventLoop（重要）

**Q6：`Channel::Tie` 的 `weak_ptr` 是干嘛的？**

**A6：** 在处理事件期间保持拥有者对象（Connection/Acceptor）存活：`HandleEvent` 前用 `tie_.lock()` 拿强引用，若拥有者已析构则直接 return，避免悬垂指针回调。见 `event_loop.cpp:140-145`。

**Q7：`Channel::HandleEvent` 的事件分发顺序？`EPOLLHUP` 为什么单独处理？**

**A7：** 先判 `EPOLLHUP` 且无 `EPOLLIN` → 直接 close；`EPOLLERR` → error；`EPOLLIN|EPOLLPRI|EPOLLRDHUP` → read；`EPOLLOUT` → write。HUP 单独判断是为了避免「对端半关闭但还有数据可读」时误关。见 `event_loop.cpp:147-160`。

**Q8：`EventLoop` 的 eventfd wakeup 机制是干嘛的？`QueueInLoop` 什么情况下需要 Wakeup？**

**A8：** 其他线程 `QueueInLoop` 投递回调时，loop 线程可能正阻塞在 `epoll_wait`，需写 eventfd 唤醒它去执行 pending functors；若已在 loop 线程且不在执行回调中（`calling_pending_`），可直接执行无需唤醒。见 `event_loop.cpp:67-79`。

**Q9：`DoPendingFunctors` 为什么用 `swap` 而不是直接遍历？**

**A9：** 用局部 vector `swap` 出 pending 队列后、在锁外执行回调，既避免长回调长期持锁阻塞其他线程投递，也避免回调里再 `QueueInLoop` 造成死锁；`calling_pending_` 标志配合 Wakeup 判断。见 `event_loop.cpp:87-96`。

### 7.5 `connection.h/.cpp` — 连接（重要）

**Q10：边沿触发下读/写循环为什么读到 EAGAIN 才停？**

**A10：** ET 模式下事件只在状态变化时通知一次，必须把 fd 读到 `EAGAIN`（内核缓冲区读空）才停止，否则剩余数据不会再触发事件、造成「丢读」。写同理，写到 EAGAIN（缓冲区满）即停，等下次 `EPOLLOUT`。见 `connection.cpp:64-98`。

**Q11：`Connection::Send` 跨线程（shard worker → IO reactor）怎么保证安全？**

**A11：** 若在 loop 线程直接 `SendInLoop`；否则捕获 `shared_from_this()` 保证连接在排队的写回调执行前存活，再 `RunInLoop` 把写操作投递到 loop 线程执行。见 `connection.cpp:29-38`。

**Q12：`HandleClose` 为什么先 `Remove` 再 close？**

**A12：** 在析构 `close(fd)` 之前先从 epoll 里 `EPOLL_CTL_DEL` 移除，否则复用的 fd 编号会和残留的注册项冲突（新连接复用同一 fd 时会收到旧事件）。见 `connection.cpp:101-108`。

**Q13：`Shutdown` 的优雅关闭怎么实现？**

**A13：** 若输出缓冲还有未发数据，置 `closing_` 并等 `HandleWrite` 把数据刷出后再 `HandleClose`；否则直接关闭。避免把尚未回包的响应丢掉。见 `connection.cpp:55-62`。

### 7.6 `acceptor.cpp` + `reactor.cpp` — 接入与反应器

**Q14：`accept4` 为什么用 `SOCK_NONBLOCK | SOCK_CLOEXEC`？fd 耗尽（EMFILE）怎么处理？**

**A14：** 直接 accept 出非阻塞 + close-on-exec 的连接，省一次 `fcntl`；fd 耗尽（EMFILE/ENFILE）时告警并停止本轮 accept，避免忙循环。见 `acceptor.cpp:33-53`。

**Q15：`IOReactor::AddConnection` 为什么用 `RunInLoop` 而不是直接创建 `Connection`？**

**A15：** Connection 的 `Establish`/Channel 操作都必须在 loop 线程执行（epoll 事件注册），所以把「创建 Connection + 注册回调 + Establish」整个调度到该 reactor 的 loop 线程执行。见 `reactor.cpp:20-35`。

## 8. persistence/（持久化）

### 8.1 `snapshot_gate.h` — 快照闸门

**Q1：`SnapshotGate` 是干什么的？为什么 fork 前要持 exclusive 锁？**

**A1：** 它是一个 reader-writer 锁（`shared_mutex`）。所有读写 shard 的任务（命令任务 + 各 worker 定时器里的淘汰/过期/rehash）执行期间持 **shared** 锁；fork 做快照（AOF rewrite / BGSAVE）前持 **exclusive** 锁。因为 `fork()` 只把调用它的那一条线程带进子进程、子进程看到 fork 瞬间的内存快照——若某个 shard worker 正把 dict 改到一半（rehash 搬迁、桶链指针半更新），子进程就读到「撕碎的中间态」。exclusive 锁保证 fork 时没有任何 shard 任务在途、快照一致；shared 锁互不排斥，正常读写仍完全并发。见 `include/mycache/persistence/snapshot_gate.h:7-24`。

**Q2：为什么「fork 只把调用它的那一条线程带进子进程」是理解 rewrite/bgsave 子进程的关键？**

**A2：** POSIX `fork` 语义：子进程是调用线程的单线程复制品，其他线程在子进程里不存在，连 `mutex` 都可能处于「被已消失线程持有」的状态。所以子进程**不能碰任何锁、不能做堆分配（malloc 锁同理）**，只能用 async-signal-safe 的系统调用 + `_exit`。这解释了为什么 `RewriteAofChild`/`WriteRdbChild` 全程无锁无分配。

### 8.2 `aof_format.h/.cpp` — AOF 格式与 CRC

**Q3：AOF 的记录格式是什么？每条记录怎么保证完整性？**

**A3：** 文件头 magic `"MYAOF001"`（8 字节），之后每条记录 = `[len(4 小端)][crc32(4 小端)][payload]`，payload 是一条命令的 RESP 数组。`len` 是 payload 字节数，`crc` 是 payload 的 CRC32，用来检测损坏/半写。见 `aof_format.h:16-33`。

**Q4：CRC32 查表为什么用 `constexpr` 常量数组，而不是函数内 `static`？**

**A4：** 函数内 `static`（magic static）有惰性初始化的 guard 变量，fork 出的子进程首次调用 `Crc32` 时可能因 guard 状态不一致而**死锁**。`constexpr` 在编译期完成初始化、无运行时 guard，子进程调用绝对安全。见 `src/persistence/aof/aof_format.cpp:12-27`。

### 8.3 `aof_writer.h/.cpp` — 落盘策略与 rewrite buffer

**Q5：三种 fsync 策略（always/everysec/no）的区别？它们共同的保证是什么？**

**A5：** `always` 每条记录 write + fsync（最安全最慢）；`everysec` 每条 write、后台线程每秒 fsync 一次（默认）；`no` 每条 write、不 fsync（交给 OS，最快）。三者共同保证：每条记录在返回前已 `write()` 到内核页缓存（进程被 `kill -9` 后仍存活）；差异只在 fsync 时机，即对断电/宕机的持久性。见 `aof_writer.h:15-25`。

**Q6：`AofWriter` 怎么保证多个 shard worker 并发追加不交错？**

**A6：** 所有 shard worker 共享同一个 `AofWriter`，`Append` 内部用 `mu_` 串行化 write，多条记录不会交错。见 `aof_writer.h:22-24`、`aof_writer.cpp:108-120`。

**Q7：`everysec` 的后台 fsync 线程怎么工作？为什么持锁 fsync？**

**A7：** `FlushLoop` 每 1s 醒来，若 `dirty_`（自上次 fsync 以来有写入）就 fsync；持锁 fsync 避免与写路径交错。见 `aof_writer.cpp:135-145`。

**Q8：rewrite 的三段协议 `BeginRewrite` / `CompleteRewrite` / `AbortRewrite` 分别做什么？**

**A8：** `BeginRewrite` 置 `rewriting_=true` 并清空 `rewrite_buffer_`（fork 前持 exclusive 锁调用，保证 buffer 只捕获 fork 之后的写入）；期间 `Append` 把记录同时追加到 `rewrite_buffer_`；`CompleteRewrite` 把 buffer 追加到 temp 文件 + fsync，再 `rename` 换掉旧日志、重开新文件（失败不 rename，旧日志完好）；`AbortRewrite` 清 buffer、`rewriting_=false`（子进程失败时调用，绝不 rename）。见 `aof_writer.cpp:147-199`。

### 8.4 `aof_recovery.cpp` — 恢复与尾部截断

**Q9：恢复时如何区分「尾部不完整（可修复）」和「中间损坏（致命）」？**

**A9：** 逐条读 `[len][crc][payload]` 校验 CRC。若记录头声称的 `len` 超出文件剩余字节 → 尾部不完整，截断丢弃；若 CRC 不匹配但该记录是**最后一条**（payload 恰好到文件尾）→ 尾部损坏，截断；若 CRC 不匹配且后面**还有**数据 → 中间损坏，致命错误拒绝启动。见 `src/persistence/aof/aof_recovery.cpp:106-163`。

**Q10：尾部截断后做什么？为什么 `kill -9` 会产生尾部不完整记录？**

**A10：** 用 `ftruncate` 把文件截到最后一个完整记录末尾，丢弃尾部半条记录。`kill -9` 可能在 `write` 到一半时终止进程，留下半条记录（len 声称的字节数 > 实际写入字节数）。见 `aof_recovery.cpp:167-177`。

**Q11：`ApplyAofCommand` 支持哪些命令？恢复时为什么只需按 `args[0]` 路由？**

**A11：** 支持 SET/DEL/HSET/HDEL/PEXPIREAT/LPUSH/RPUSH/LPOP/RPOP。恢复是单线程、按 key 哈希 `ShardIndex(args[0])` 直接定位 shard，无需跨线程路由。见 `aof_recovery.cpp:184-232`、`persistence_manager.cpp:32-37`。

### 8.5 `aof_rewrite.cpp` — fork 子进程 + COW

**Q12：为什么 AOF 需要 rewrite？rewrite 输出什么？**

**A12：** AOF 是 append-only，反复 SET 同一 key 会累积大量冗余记录、文件越滚越大。rewrite 用**当前内存状态**重写一份紧凑 AOF：每个 key 一条当前值的 SET/HSET/RPUSH + PEXPIREAT，丢掉历史中间态。见 `src/persistence/aof/aof_rewrite.cpp:213-230`。

**Q13：rewrite 子进程为什么全程无堆分配、无锁？哪些操作是安全的？**

**A13：** fork 后子进程只剩单线程，但可能继承「被其他线程持有」的 mutex；malloc 的锁同理。所以子进程只用 async-signal-safe 的 `::open/::write/::fsync/::close/::_exit`，数字格式化在栈上做（`U64toa`/`I64toa`），缓冲由父进程 fork 前预分配（`out_buf`/`rec_buf`）。见 `aof_rewrite.h:12-21`。

**Q14：列表在 rewrite 时为什么要分块（`kChunk = 128`）？**

**A14：** 每个 RPUSH 记录最多 128 个元素，限制单条记录大小不超过 rec 缓冲（1 MiB），避免超长列表一条记录塞爆缓冲。见 `aof_rewrite.cpp:180-201`。

**Q15：rewrite 用 COW 怎么和 fork 结合？父进程 fork 后继续写怎么不丢？**

**A15：** fork 后子进程拥有父进程内存的 COW 快照，父进程继续写只复制被改页、不影响子进程看到的旧快照。子进程基于快照写 compact AOF 到 temp；同时父进程 `BeginRewrite` 已开启 `rewrite_buffer_`，fork 后的新写入同时追加到 buffer，最后 `CompleteRewrite` 把 buffer 补到 temp 末尾再 rename——二者衔接无缝隙。见 `persistence_manager.cpp:102-116`。

### 8.6 `persistence_manager.cpp` — 编排与 SAVE/BGSAVE

**Q16：`Init` 的启动顺序？（AOF 恢复 vs RDB 加载）**

**A16：** 先 `CreateIOEngine`；若 `appendonly` 开 → `ReplayAndOpen`（先 `AofRecovery::Replay` 逐条重放+修复尾部，再 `AofWriter::Open` 追加打开）；否则 `LoadRdbIfPresent`（有 `dump.rdb` 就 `RdbLoad`，无则空库启动）。都在 shard worker 启动前单线程完成，可直接写 shard。见 `persistence_manager.cpp:67-76`。

**Q17：`StartRewrite` 的完整流程？为什么 fork 前后都持 exclusive 锁？**

**A17：** 检查 AOF 开启/无子进程/未在 rewrite → 预分配子进程缓冲 → 持 exclusive 锁 → `BeginRewrite` → `fork` → 解锁。子进程 `RewriteAofChild` 写 temp 后 `_exit`；父进程记 `child_pid`/`child_kind`。持锁是为了排空在途 shard 任务、保证 COW 快照一致；`BeginRewrite` 在锁内调用，保证 rewrite buffer 只捕获 fork 之后的写入。见 `persistence_manager.cpp:83-128`。

**Q18：`SAVE` 和 `BGSAVE` 的区别？**

**A18：** `SAVE` **同步阻塞**：持 exclusive 锁、在调用线程直接 `RdbWriteSnapshot`（此时无 worker 改 shard，可自由分配），完成才返回；`BGSAVE` **异步**：持锁 `fork` 子进程写快照、父进程立即返回，SIGCHLD 收割后 `OnChildExit` 里 rename 完成。见 `persistence_manager.cpp:130-192`。

**Q19：`OnChildExit` 怎么收割子进程并完成收尾？失败怎么办？**

**A19：** SIGCHLD 处理里调 `OnChildExit(pid, status)`：先匹配 pid、清 child 状态；用 `WIFEXITED/WEXITSTATUS` 判断成功。rewrite 成功 → `CompleteRewrite(temp)`，失败 → `AbortRewrite` + unlink temp（保留旧日志）；bgsave 成功 → `rename(temp, rdb)`，失败 → unlink temp（保留旧快照）。见 `persistence_manager.cpp:203-228`。

### 8.7 `rdb_writer/loader` — RDB 快照

**Q20：RDB 的简化格式长什么样？**

**A20：** `[magic "MYRDB001"][version 0x01]`，之后每个 key 一条记录 `[len][crc][payload]`，最后 `[EOF 0xFF]`。payload = `[type][has_ttl][ttl_ms?][key_len][key][value...]`，string/hash/list 三种 type 各自编码。见 `rdb_writer.h:10-26`。

**Q21：RDB 与 AOF 的本质区别？**

**A21：** RDB 是某个时刻的**全量二进制快照**（紧凑、加载快，但可能丢最后一段写入）；AOF 是 append-only 的**命令日志**（可每写持久化、恢复慢、文件会膨胀需 rewrite）。项目里 RDB 用 SAVE/BGSAVE 触发，AOF 用每次写命令追加 + 三种 fsync 策略。

> 进阶：运行时还原 `LOAD` 命令（DEBUG RELOAD 风格）——并发/原子性/AOF 一致性三个难点与三段面试话术，见 [`docs/load-runtime-restore.md`](load-runtime-restore.md)。

## 9. protocol/（RESP 协议）

### 9.1 `resp_value.h/.cpp` — 通用 RESP 值

**Q1：`RespValue` 怎么表示 6 种 RESP 类型？为什么用单一 struct 而不是继承多态？**

**A1：** 用一个 struct 含 `type` + `str` + `integer` + `array` 四个字段，按 `type` 取不同字段（简单类型用 `str`，整数用 `integer`，数组用 `array`，null 用 `type=kNull`）。单一 struct 省去虚函数/继承的开销，配合 `std::vector` 存储解析结果简单高效。见 `include/mycache/protocol/resp_value.h:9-26`。

### 9.2 `resp_encoder.cpp` — 序列化

**Q2：`EncodeResp` 怎么处理嵌套数组？null 怎么编码？**

**A2：** 递归：array 先输出 `*count\r\n`，再对每个元素递归 `EncodeResp`；bulk 是 `$len\r\n<bytes>\r\n`；null 编码成 `$-1\r\n`。见 `src/protocol/resp_encoder.cpp:9-47`。

### 9.3 `resp_parser.h/.cpp` — 有状态增量解析器（重要）

**Q3：为什么解析器要「有状态增量式」而不是每次从 0 重扫？**

**A3：** TCP 是字节流，一条命令可能分多次 `recv` 到达（半包）。若每次重扫，前面已解析的字节要反复拷回、退化成 O(n²)。有状态解析器记住进度、把已消费的前缀拷进内部状态（`line_`/`current_.str`），下次从「未解析处」续接，O(n)。见 `resp_parser.h:14-26`。

**Q4：`Parse` 的三种返回值 kOk / kIncomplete / kError 各自的契约是什么？**

**A4：** `kOk`：完整顶层值已出、`*consumed`=消耗字节数、解析器回到 idle 可继续下一条（流水线）；`kIncomplete`：`*consumed` 可能 >0，表示「已安全拷贝进内部状态、可丢弃的前缀」，调用方必须 `retrieve(*consumed)`；`kError`：`*consumed==0`、内部状态已脏，必须 `Reset()` 或关连接。**无论 kOk 还是 kIncomplete 都要 `retrieve(*consumed)`**。见 `resp_parser.h:18-26`。

**Q5：嵌套数组怎么解析？`Commit` 的作用？**

**A5：** 用 `stack_` 栈存「正在填充的数组帧」（`value` + `remaining` 还剩几个元素）；遇到 `*n` 压栈；每完成一个元素就 `Commit` 向上提交——栈空则顶层值完成写 `*out`，否则填入栈顶数组、`remaining` 减到 0 就弹出整帧继续向上。见 `src/protocol/resp_parser.cpp:40-53`。

**Q6：跨读拆分的「行」和「bulk 体」怎么续接？为什么不会重扫？**

**A6：** 行没收到 `\n` 时把已读字节 `append` 进 `line_` 并消费（下次从新位置续接）；bulk 体同样把已读部分 `append` 进 `current_.str` 并记录 `bulk_remaining_` 还剩几字节。已拷贝进内部状态的字节可安全 `retrieve` 丢弃，下次喂入从「未解析处」开始。见 `resp_parser.cpp:99-113`、`202-215`。

**Q7：有哪些防护性上限？各自防什么？**

**A7：** `kMaxBulkLength=512MB`（与 Redis 一致，防超大 bulk 吃内存）、`kMaxArrayElements=1M`（防超大数组）、`kMaxNestingDepth=128`（防嵌套过深栈溢出）、`kMaxLineLength=64KB`（防无 `\n` 时 `line_` 无限增长）。见 `resp_parser.cpp:14-17`。

**Q8：为什么命令之间会跳过杂散的 CR/LF？**

**A8：** `redis-cli --pipe` 等会在命令流末尾追加换行；跳过 `\r`/`\n` 让这些「命令间空白」不触发协议错误。见 `resp_parser.cpp:68-72`。

**Q9：null bulk 和 null array 怎么表示？**

**A9：** `$-1\r\n`（bulk 长度 -1）表示 null bulk，`*-1\r\n` 表示 null array，都解析成 `type=kNull` 的 `RespValue`（`Commit` 一个默认值）。见 `resp_parser.cpp:148-158`、`167-173`。

## 10. server/（编排·路由）

### 10.1 `command_router.h/.cpp` — 路由

**Q1：`ShardIndex` 怎么算？为什么用 `DictHash` 而不是 `std::hash`？**

**A1：** `DictHash(key) % shard_count`，`DictHash` 是 MurmurHash64A。用它是为了**确定且分布均匀**：AOF 恢复/多 key 扇出都要按同一个 key 稳定落到同一 shard，`std::hash` 的实现跨平台/跨版本不保证一致。见 `src/server/command_router.cpp:25-27`。

**Q2：`RouteTo` 为什么在任务执行时包一层快照闸门 shared 锁？**

**A2：** 保证 fork 做快照（AOF rewrite / BGSAVE）时看不到「半修改的 shard」——任务执行期间持 shared 锁，fork 前持 exclusive 锁排空所有在途任务。见 `command_router.cpp:33-41`。

**Q3：`ShardAt` 为什么注释说「仅供启动阶段单线程使用」？**

**A3：** 它直接返回 shard 指针、不加锁不路由，只用于 AOF 恢复/RDB 加载等 shard worker 启动**前**的单线程场景；运行时并发访问必须走 `Route`/`RouteTo` 提交到 shard 线程。见 `command_router.h:28-29`。

### 10.2 `shard_worker.h/.cpp` — shard 线程

**Q4：`ShardWorker` 的线程模型？为什么每个 shard 一个线程而不是共享线程池？**

**A4：** 每个 ShardWorker 拥有一个 Shard + 一个 EventLoop + 一个线程，**串行**执行派发到它的任务——因此单个 shard 内部无需加锁（所有对该 shard 的写都在它自己的线程串行执行），只有跨 shard 才需要并发控制。这就是「按 shard 分片 + 每 shard 单线程」的核心思想，等价于把 Redis 的单线程模型复制 N 份。见 `include/mycache/server/shard_worker.h:13-27`。

**Q5：timerfd 100ms 定时器驱动哪些后台任务？**

**A5：** `RefreshLruClock`（刷新 LRU 时钟）、`ActiveExpireCycle`（主动过期采样删除）、`EvictIfNeeded`（超限后台兜底淘汰）、`IncrementalRehash(1)`（空闲推进 rehash）。都持快照闸门 shared 锁，与 Redis 的 `serverCron`/`databasesCron` 对齐。见 `src/server/shard_worker.cpp:65-77`。

**Q6：`Submit` 为什么用 `RunInLoop`？**

**A6：** 把任务投递到 shard 线程执行：若调用方已在 shard 线程则直接执行，否则排队 + eventfd 唤醒，保证所有对该 shard 的访问都在其线程上串行。见 `shard_worker.cpp:44-47`。

### 10.3 `server.h/.cpp` — Server 编排（重要）

**Q7：`Start` 的启动顺序？为什么 AOF 恢复在启动 worker 线程之前？**

**A7：** 忽略 SIGPIPE → `MarkStart` → `RefreshLruClock` → `CreateSharedObjects`（共享整数池）→ `pm_.Init`（AOF 恢复/RDB 加载，在 shard worker 启动前单线程直接写 shard）→ 启动 IO 反应器 → 启动 shard worker → 建 signal/child eventfd + acceptor → `main_loop_.Loop`。恢复在 worker 启动前是为了单线程直接写 shard、无需锁。见 `src/server/server.cpp:83-139`。

**Q8：`OnNewConnection` 怎么把连接分给多个 IO 反应器？**

**A8：** round-robin：`io_reactors_[next_reactor_ % io_reactors_.size()]`，`next_reactor_` 自增，均匀分摊连接负载。见 `server.cpp:170-181`。

**Q9：`OnMessage` 里 kIncomplete 为什么也要 `retrieve(consumed)`？**

**A9：** 有状态增量解析半包时 `consumed` 也可能 >0（已安全拷贝进内部状态的前缀），必须丢弃，否则下次会重复解析同一段。见 `server.cpp:183-203`。

**Q10：`DispatchCommand` 的三分支（多 key / 单 key / 无 key）分别怎么走？**

**A10：** `last_key != 0`（多 key：DEL/EXISTS/MGET/MSET）→ 在 IO 线程执行、handler 内按 shard 扇出、`ctx.db=nullptr`；`first_key > 0`（单 key）→ 按 key 哈希路由到 shard 线程执行；否则（无 key：PING/ECHO/INFO）→ IO 线程直接执行。见 `server.cpp:234-266`。

**Q11：信号处理为什么用 eventfd + 主循环读回，而不是直接在信号处理器里 `Stop`？**

**A11：** 信号处理器只能做 async-signal-safe 操作，`Stop` 涉及加锁/join/close 等非安全操作；所以信号处理器只往 eventfd 写一个字节，主循环读回后在线程上下文里执行 `Stop`。SIGCHLD 同理，`waitpid` 收割放到 `HandleChildExit`。见 `server.cpp:35-51`、`150-168`。

**Q12：一个 SIGCHLD 信号可能对应多个已退出子进程，怎么处理？**

**A12：** 用 `while (waitpid(-1, &status, WNOHANG) > 0)` 循环收割，直到没有更多已退出子进程（因为信号会合并）。见 `server.cpp:158-168`。

**Q13：为什么忽略 SIGPIPE？**

**A13：** 向已关闭的 socket 写会触发 SIGPIPE，默认动作是终止进程；服务器不应因客户端断开而崩溃，所以 `SIG_IGN` 让 `write` 返回 `EPIPE` 错误码由程序处理。见 `server.cpp:85`。

## 11. storage/（存储核心）

### 11.1 `dict/` — 字典与渐进式 rehash（重要）

**Q1：`Dict` 的底层结构？「双哈希表」怎么用？**

**A1：** 两个 `HashTable`（`ht[0]` 服务请求、`ht[1]` 是 rehash 目标表），每张表是桶数组（`DictEntry** table`）+ `size`（2 的幂）+ `sizemask`（size-1）+ `used`；每个桶是一条 `DictEntry` 链（key/val/next）。索引 `idx = hash & sizemask`。见 `dict.h:11-15`、`hash_table.h:11-16`。

**Q2：为什么桶数取 2 的幂？`idx` 怎么算？**

**A2：** 2 的幂让 `idx = hash & (size-1)` 用**位与替代取模**，比 `%` 快；扩容/迁移到 ht[1] 时同样 `& sizemask` 即可。见 `hash_table.h:14`。

**Q3：`DictType` 是干嘛的？`key_dup/val_dup/key_destructor/val_destructor` 各是什么用？**

**A3：** 类型参数化，由使用者提供键值的 `hash/equal/dup/destructor` 操作，让 `Dict` 通用（可存 sds→Object*、sds→时间戳等）。`equal` 返回 bool（与 Redis 的 `dictCompareKeys` 约定相反，贴 C++ 习惯）。`dup` 在写入时拷贝键值，`destructor` 在删除时释放。见 `dict.h:20-27`。

**Q4：渐进式 rehash 的核心思想？为什么不做一次性全量迁移？**

**A4：** 扩容时分配 `ht[1]`、置 `rehashidx=0`，之后**每次写操作**（Add/Delete/Replace）通过 `RehashStep` 迁移少量桶（每次 1 桶），把 O(N) 的全量迁移摊薄到每次操作 O(1)，避免一次性 rehash 卡住服务器（Redis 经典设计）。见 `dict.h:35-41`、`rehash.cpp:103-105`。

**Q5：rehash 期间怎么查 key？为什么 `Find` 有两个版本？**

**A5：** 两表都查：先查 ht[0]（`hash & sizemask`），再查 ht[1]（若在 rehash）。`Find`（const）**只读、不推进 rehash**，供共享锁读路径（多线程读快照）；`FindAndRehash` 先 `RehashStep` 再查两表，供单线程 Shard 读路径（对齐 Redis `dictFind`）。见 `dict.cpp:143-168`。

**Q6：扩容/收缩的触发阈值？**

**A6：** 扩容 `ExpandIfNeeded`：`ht[0].used >= ht[0].size`（负载因子 ≥1）时 `Expand(used*2)`；收缩 `ShrinkIfNeeded`：`used*8 < size` 且 size>初始大小（负载因子 <1/8）时 `Resize`。见 `rehash.cpp:38-52`。

**Q7：`Rehash` 怎么限制单次耗时？`empty_visits` 干嘛的？**

**A7：** `Rehash(n)` 迁移最多 n 个非空桶，但最多跳过 `n * kRehashEmptyVisitFactor(=10)` 个空桶就返回，避免在稀疏表上空转耗时过长；`RehashMilliseconds` 用 steady_clock 限时。见 `rehash.cpp:54-101`。

**Q8：`AddRaw` 在 rehash 中把新 key 写进哪张表？为什么？**

**A8：** 若正在 rehash，新 key 写入 `ht[1]`（避免加到即将被清空的 ht[0] 导致漏迁移）；否则写 `ht[0]`。见 `dict.cpp:108-118`。

**Q9：`DictHash` 是 MurmurHash64A，为什么用 `memcpy` 读 8 字节块？**

**A9：** 通过 `memcpy` 以 8 字节块读取，避免未对齐访问（sds 数据可能非 8 字节对齐，直接强转 `uint64_t*` 是 UB）。见 `dict.cpp:9-44`。

### 11.2 `string/sds` — 简单动态字符串（重要）

**Q10：SDS 的内存布局？`sds` 指针指向哪？`s[-1]` 是什么？**

**A10：** 头部（`sdshdrN`：len/alloc/flags）紧邻数据之前，返回给用户的 `sds` 指向数据缓冲区（与 C 字符串兼容、始终 `\0` 结尾但内容可含 `\0`）；`s[-1]` 是 flags 字节，据此反推头部类型和大小。见 `sds.h:8-21`。

**Q11：为什么有 `sdshdr8/16/32/64` 四种头部？**

**A11：** 按字符串长度选最小头部，短字符串用 1 字节 len 省内存，长字符串用 8 字节；`sdsReqType` 选类型，`sdsMakeRoomFor` 在类型升级时搬数据。见 `sds.h:29-68`。

**Q12：SDS 相比 C 字符串的优势？**

**A12：** O(1) 取长度（len 前缀，不用 strlen）；二进制安全（内容可含 `\0`，用 len 界定）；预分配（append 时 2 倍增长、超 1MB 每次 +1MB）摊薄 realloc；记录 alloc 容量杜绝缓冲区溢出。见 `sds.h:22-24`。

**Q13：`sdsMakeRoomFor` 的预分配策略？头部类型升级怎么处理？**

**A13：** avail 不足时 `newlen = len+addlen`，<1MB 翻倍、≥1MB 每次 +1MB；若类型不变原地 `realloc`，类型升级则分配新块、`memcpy` 搬移、free 旧块。见 `sds.cpp:48-80`。

**Q14：为什么头结构用 `__attribute__((packed))`？**

**A14：** 让 flags 字节始终紧邻数据之前（即 `s[-1]`），与 Redis sds 布局一致，避免编译器对齐填充导致 `s[-1]` 拿到错误位置。见 `sds.h:43-48`。

**Q15：`sdscmp` 为什么二进制安全？长度不同怎么比？**

**A15：** 用 `sdslen` 拿各自长度，`memcmp` 比较 `min(len)` 字节，相等则按长度分胜负——不是 `strcmp` 那种遇 `\0` 停。见 `sds.cpp:111-118`。

### 11.3 `object/` — robj 与编码（重要）

**Q16：`Object`（robj）的 `kInt`/`kString` 两种编码分别怎么存值？**

**A16：** `kInt`：long long 直接嵌在 `ptr` 字段（`ObjectPtrToLongLong`/`LongLongToObjectPtr` 互转），不单独分配；`kString`：`ptr` 指向独立 sds。见 `object.h:26-34`。

**Q17：为什么要把字符串编码成整数？省什么？**

**A17：** 编码成 `kInt` 后省掉 sds 的头部+数据分配（值嵌在 Object 内）；且 0..9999 还能复用共享整数池，进一步省内存。这是 Redis `tryObjectEncoding` 的核心优化。

**Q18：共享整数池（`kSharedIntegers=10000`）的原理？refcount 怎么处理？**

**A18：** 启动时 `CreateSharedObjects` 一次性创建 0..9999 共 10000 个对象，`refcount` 设为 `INT_MAX`（`kSharedRefCount`）；`IncrRefCount` 判断 `refcount < INT_MAX` 才递增（避免有符号溢出），`DecrRefCount` 判断 `== INT_MAX` 直接 return（永驻不释放）。见 `object.h:39-45`、`string_object.cpp:91-98`。

**Q19：`TryObjectEncoding` 什么时候复用共享整数？什么时候就地转 int？**

**A19：** `len<=20` 且能严格解析为 long long 时：若 `allow_shared` 且 `value∈[0,10000)` 且池已建，释放原对象返回共享对象；否则 `sdsfree` 后原地改写为 int 编码。`allow_shared` 在 LRU 淘汰策略下为 false（见 §3）。见 `string_object.cpp:100-125`。

**Q20：`StringToLongLong` 为什么比 `strtoll` 严格？拒绝什么？**

**A20：** 只接受可选前导 `-` 和十进制数字，拒绝前导零（`"012"`）、`'+'`、空白、空串、`"-0"`；用 unsigned long long 累加 + 溢出检查，最后处理 LLONG_MIN 边界（`0x8000000000000000`）。见 `string_object.cpp:12-58`。

**Q21：`DecrRefCount` 归零后怎么释放？为什么按 type 分派？**

**A21：** `kString+kString` 编码释放 sds；`kHash` 调 `ReleaseHashObject`（析构内部 dict）；`kList` 调 `ReleaseListObject`（释放链表节点）；最后 `zfree` 自身。按 type/encoding 分派，因为不同对象的内部分配结构不同。见 `object.cpp:14-33`。

### 11.4 `memory/` — 带统计的分配器

**Q22：`zmalloc` 怎么统计内存？为什么用 `malloc_usable_size` 而不是请求的 size？**

**A22：** 分配后用 `malloc_usable_size(p)` 拿系统实际分配的可用于量（含对齐/内部开销），加到原子 `g_used_memory`；`zfree` 减掉；`zrealloc` 用 `UsableSize` 前后差值调整。用 usable size 统计「真实占用」，比请求 size 更贴近 RSS。见 `allocator.cpp:30-57`。

### 11.5 `db/dict_types.h` — 共享的 dict 类型

**Q23：`SdsObjectDictType` 的 `val_dup`/`val_destructor` 是什么？为什么用引用计数？**

**A23：** `val_dup = ObjectDup`（`IncrRefCount`）、`val_destructor = ObjectFree`（`DecrRefCount`）。dict 存 value 时只增引用计数、不深拷贝对象；删 entry 时减计数、归零才真正释放——高效，且多个 key 共享同一对象也安全。见 `dict_types.h:28-48`。

**Q24：`expires_` 为什么用 `SdsRawDictType` 而不是 `SdsObjectDictType`？**

**A24：** `expires_` 的 value 是 int64_t 到期时间戳（用 `void*` 强转存储），不是 `Object*`，所以 `val_dup/val_destructor` 为 nullptr（裸指针，不增/减引用计数）。见 `dict_types.h:51-60`。

### 11.6 `db/shard` — 单 shard 存储（重要）

**Q25：`Shard` 的两个 dict 分别存什么？**

**A25：** `dict_` 存 sds→Object*（主键空间）；`expires_` 存 sds→int64_t 到期时间戳（存于 `void*`，用 `reinterpret_cast<intptr_t>` 强转）。见 `shard.h:93-94`。

**Q26：`EvictIfNeeded` 的淘汰逻辑？`max_iter=1000` 干嘛？**

**A26：** `maxmemory<=0` 直接 true；`noeviction` 超限返回 false；否则 `while used_memory() > limit` 循环 `EvictOne`，最多 1000 次防空转，最后返回 `used_memory() <= limit`。见 `shard.cpp:30-44`。

**Q27：per-shard 淘汰怎么和全局 maxmemory 等价？（面试重点）**

**A27：** 每个 shard 持有**全局** maxmemory 上限，在自己的写路径上检查**全局** `used_memory()` 是否超限、并**只从自己哈希分到的 key 里**淘汰。因为 key 均匀哈希分布，各 shard 持有的 key 数量近似相等，各自独立「淘汰自己那部分直到全局不超限」就近似等价于全局淘汰；但精确的淘汰集合可能与「单一全局 LRU」有差异（某个 shard 可能淘汰了较新 key，而别的 shard 还留着更老的）。这是分片下的近似，详见 `docs/eviction.md`。见 `shard.h:22-24`、`shard.cpp:30-44`。

**Q28：`PickVictim` 的 LRU 怎么挑？volatile-lru 为什么去 `dict_` 查对象？**

**A28：** LRU 策略遍历候选找 `lru` 最小（最久未访问）的 key；volatile 时候选来自 `expires_`（val 是时间戳不是对象），需 `dict_.FindAndRehash` 去主 dict 查对象拿 lru。random 则随机取一个候选。见 `shard.cpp:76-103`。

**Q29：惰性过期 vs 主动过期？三个函数各管什么？**

**A29：** **惰性**：每次访问（`LookupValue`）先 `ExpireIfNeeded` 检查是否到期、到期即删，保证「读到的一定是活的」；**主动**：`ActiveExpireCycle` 定时（100ms）随机采样 20 个桶删到期 key，保证「不读的也能被回收」。二者结合是 Redis 经典双管齐下。见 `shard.cpp:114-131`、`437-467`。

**Q30：`SET` 为什么先删 `expires_` 里的 TTL？**

**A30：** Redis 语义：SET 会清除 key 的 TTL。所以 `Set` 里 `expires_.Delete(k)`。见 `shard.cpp:151`。

**Q31：`ScanSnapshot` 为什么无锁无分配、不推进 rehash？**

**A31：** 它被 fork 出的子进程（AOF rewrite / RDB bgsave）用来遍历 COW 快照，子进程不能加锁/分配，所以只读遍历两张表、用只读的 `expires_.Find`（不推进 rehash）、不改任何状态。见 `shard.cpp:477-493`。

**Q32：hash 清空 / list 清空后为什么删 key？**

**A32：** Redis 语义：集合类型的 key 在最后一个字段/元素被删后，整个 key 删除（不留空对象）。`HDel` 里 `hd->Size()==0` 删 key，`LPop`/`RPop` 里 `l->len==0` 删 key。见 `shard.cpp:240`、`329`。

**Q33：`LRange` 怎么处理负下标？**

**A33：** `start/stop` 为负时转成 `len+start` / `len+stop`，再夹到 `[0, len-1]`，`start>stop` 或 `start>=len` 返回空。见 `shard.cpp:369-383`。

### 11.7 `collection/` — hash 与 list

**Q34：list 为什么用朴素双向链表而不是 `std::deque`？**

**A34：** `std::deque` 是分块数组，两端 push/pop 虽 O(1)，但节点内存不走 zmalloc、无法被 `used_memory()` 精确统计；朴素双向链表每个节点 `zmalloc` 分配、可精确统计。Redis 用 quicklist（ziplist+链表混合）优化内存，这里简化为纯链表。见 `list.h:10-23`。

**Q35：`ListNode` 的 `prev/next` 为什么在 `NewNode` 里显式置空？**

**A35：** `zmalloc` 是 `malloc`（不调构造函数），不会执行默认成员初始化器，必须显式置空 `prev/next`，否则是未初始化指针。见 `list.cpp:11-17`。

**Q36：`ListPopFront` 返回 sds 时为什么把 `node->value` 置 nullptr？**

**A36：** sds 所有权转移给调用方、由调用方 `sdsfree`，node 本身 `zfree`；置 nullptr 明确「不再由链表持有」的语义，避免二次释放。见 `list.cpp:71-82`。

## 12. 入口与构建

### 12.1 `src/main.cpp` — 进程入口

**Q1：`main` 的启动流程？**

**A1：** 读配置（默认 `conf/mycache.conf`，或 `argv[1]`）→ 映射 `log_level` 到 `LogLevel` → `Logger::Init` → 构造 `Server` → `Start()`（阻塞，内部跑主循环，返回时已优雅关闭）→ `Logger::Shutdown`。见 `src/main.cpp:8-28`。

### 12.2 `CMakeLists.txt` — 构建

**Q2：为什么 `GLOB_RECURSE` 要 `REMOVE_ITEM main.cpp`？**

**A2：** `main.cpp` 是入口，单独编进 `mycache-server` 可执行文件，不放进 `mycache_core` 静态库——库是给 server/tests/bench 共用的核心逻辑。见 `CMakeLists.txt:20-23`。

**Q3：liburing 和 jemalloc 是怎么「可选探测」的？**

**A3：** liburing 用 `find_library(URING_LIBRARY uring)`，找到就定义 `MYCACHE_HAVE_IO_URING` 并链接；jemalloc 用 `find_file` 匹配 `libjemalloc.so` / `.so.2` + multiarch HINTS（`find_library` 匹配不了带版本号的 `.so.2`），找到就链接。都找不到就静默回退（posix / glibc malloc）。见 `CMakeLists.txt:28-57`。

**Q4：为什么 TSan / Sanitizer 构建不链接 jemalloc？**

**A4：** TSan/ASan 需要拦截分配器（malloc/free 符号）来检测数据竞争/越界，链接 jemalloc 会干扰拦截。见 `CMakeLists.txt:41`。

**Q5：`CONFIGURE_DEPENDS` 的作用？**

**A5：** 让 `file(GLOB_RECURSE)` 在新增/删除 `.cpp` 文件时自动重新运行 CMake 重新生成，规避 GLOB 不会自动感知文件变化的老问题。见 `CMakeLists.txt:20`。

**Q6：静态库 + 可执行文件怎么组织？测试/基准分别是什么 target？**

**A6：** `mycache_core` 静态库（所有 `src/*.cpp` 除 main.cpp）+ `mycache-server`（main.cpp 链接 core）+ `mycache_tests`（15 个 gtest 单测文件）+ `mycache_bench` / `mycache_aof_bench` / `mycache_io_bench`（基准）。见 `CMakeLists.txt:59-95`。

---

## 附：全项目一句话串联（面试速记）

启动 `main` → `Server::Start` 先 `PersistenceManager::Init`（AOF 恢复 / RDB 加载，单线程直写 shard）→ 起 N 个 IO reactor + N 个 shard worker → 主 loop `accept` 分发连接 → 连接上 `RespParser` 增量解析出命令 → `DispatchCommand` 三分支（多 key 扇出 / 单 key 路由 / 无 key 直接执行）→ `CommandRouter::Route` 投递到 shard 线程（持快照闸门 shared 锁）→ `Shard` 里 `LookupValue`（惰性过期 + 刷新 LRU）→ `Dict` 双表渐进 rehash → `AofWriter` 按 fsync 策略落盘 → 后台 `timerfd` 100ms 驱动 LRU 时钟/主动过期/兜底淘汰/空闲 rehash；BGREWRITEAOF/BGSAVE 持 exclusive 锁 `fork`，子进程无锁无分配写紧凑快照（COW），父进程 `SIGCHLD` 收割后 rename 完成。

