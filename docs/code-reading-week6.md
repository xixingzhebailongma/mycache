# Week 6 代码阅读指南：AOF Rewrite + RDB + io_uring

本文是 Week 6 改动的**代码阅读地图**：讲「代码在哪、按什么顺序读、每段在干什么、为什么
要这么写」。读完并做完末尾的自查题、达到第 8 节的达标标准，这周的东西才算真正掌握。

配套高层「是什么/为什么」见 `docs/persistence.md`（Week 4 AOF）与本文第 4 节；本文只讲
Week 6 新增的三块：**AOF rewrite**（日志压缩）、**RDB**（快照）、**IOEngine**（可切换 I/O）。

---

## 1. 改动清单

### 新增（核心）
| 文件 | 作用 |
|------|------|
| `include/mycache/persistence/snapshot_gate.h` | 快照闸门（`shared_mutex`），fork 前排空在途 shard 任务 |
| `include/mycache/persistence/aof/aof_rewrite.h` + `src/persistence/aof/aof_rewrite.cpp` | rewrite 子进程入口 + 无分配 RESP 编码器 |
| `include/mycache/persistence/persistence_manager.h` + `src/persistence/persistence_manager.cpp` | 持久化协调器：AOF/RDB 的打开、fork、SIGCHLD 收割、收尾 |
| `include/mycache/persistence/rdb/rdb_writer.h` + `src/persistence/rdb/rdb_writer.cpp` | RDB 二进制格式 + 无分配写入 |
| `include/mycache/persistence/rdb/rdb_loader.h` + `src/persistence/rdb/rdb_loader.cpp` | RDB 解析 + 加载到 shard |
| `include/mycache/io/io_engine.h` | `IOEngine` 抽象接口 + `CreateIOEngine` 工厂 |
| `include/mycache/io/posix_io.h` + `src/io/posix_io.cpp` | `PosixIO`：直接封装 libc 文件 I/O |
| `include/mycache/io/uring_io.h` + `src/io/uring_io.cpp` | `UringIO`：liburing 实现（`MYCACHE_HAVE_IO_URING` 时编译） |

### 新增（测试/工具）
| 文件 | 作用 |
|------|------|
| `tests/unit/week6_test.cpp` | 6 个 gtest：RDB 往返/损坏、rewrite 子进程往返、list 重放、IOEngine |
| `benchmark/io_bench.cpp` | IOEngine write+fsync 压测（posix vs io_uring） |
| `scripts/rewrite_test.sh` | rewrite 期间并发写入不丢 + kill -9 恢复 |
| `scripts/rdb_test.sh` | SAVE/BGSAVE + 重启从 dump.rdb 恢复 |

### 修改
| 文件 | 改动 |
|------|------|
| `src/persistence/aof/aof_writer.cpp` / `.h` | 走 `IOEngine`；新增 `BeginRewrite/CompleteRewrite/AbortRewrite` + `rewrite_buffer_` |
| `src/persistence/aof/aof_recovery.cpp` | `ApplyAofCommand` 补上 list 命令重放（顺带修的 bug） |
| `src/persistence/aof/aof_format.cpp` | CRC 表从函数级 `static` 改成 `constexpr`（fork 安全） |
| `src/storage/db/shard.cpp` / `.h` | 新增 `ScanSnapshot`（无锁无分配遍历，供子进程快照） |
| `src/server/command_router.cpp` | `Route` 的任务执行包一层快照闸门 shared 锁 |
| `src/server/shard_worker.cpp` | `OnTimer` 的过期/淘汰/rehash 也持 shared 锁 |
| `include/mycache/command/command_context.h` | 新增 `PersistenceManager* pm` 指针 |
| `src/command/server_commands.cpp` | 新增 `BGREWRITEAOF` / `SAVE` / `BGSAVE` 命令 |
| `src/server/server.cpp` / `.h` | `Server` 改持 `PersistenceManager`；新增 SIGCHLD 收割 |
| `include/mycache/common/config.h` | 新增 `dbfilename` / `io_engine` + `RdbPath()` |
| `CMakeLists.txt` | `find_library(uring)` 探测 + 挂新测试/压测 |

---

## 2. 一张图看懂三条数据流

### rewrite（运行时）
```
客户端 BGREWRITEAOF（无 key，IO 线程直接执行）
   └─► BgRewriteAofCommand → pm->StartRewrite()        (persistence_manager.cpp:83)
         ├─► gate.LockExclusive()                       // 排空在途 shard 任务
         ├─► aof_->BeginRewrite()                       // rewriting_=true + 清 buffer
         ├─► fork()
         │     ├─ 子进程：RewriteAofChild → 遍历各 shard 无分配编码 → 写 temp → fsync → _exit(0)
         │     └─ 父进程：记录 child_pid_，立即返回 "+Background ... started"
         └─► gate.UnlockExclusive()                     // worker 恢复；此后写入同时进 rewrite_buffer_

（子进程退出）
   └─► SIGCHLD → ChildSignalHandler 写 eventfd          (server.cpp:45)
         └─► 主循环 HandleChildExit → waitpid → pm->OnChildExit   (server.cpp:158)
               └─► aof_->CompleteRewrite(temp)          (aof_writer.cpp:153)
                     ├─ 把 rewrite_buffer_ 追加到 temp + fsync
                     ├─ rename(temp, aof)                // 原子替换
                     └─ close 旧 fd → 重开新文件 → 清 buffer/rewriting_=false
```

### RDB（SAVE / BGSAVE）
```
SAVE（同步）：Save() → gate.LockExclusive() → 单线程 RdbWriteSnapshot → rename → Unlock
BGSAVE（异步）：BgSave() → gate.LockExclusive() → fork → 子进程 WriteRdbChild → _exit
                  → 父进程 SIGCHLD → FinishBgSave → rename(temp, dump.rdb)

启动加载：pm->Init() → appendonly ? ReplayAndOpen(AOF) : LoadRdbIfPresent(RDB)
```

### IOEngine
```
AofWriter::Append / Fsync / Open ...
   └─► io_->Write / Fsync / Open / Pread / Ftruncate / Close   (io_engine.h:19-24)
         ├─► PosixIO：直接 ::write / ::fsync / ...
         └─► UringIO：io_uring 提交 SQE + 等待 CQE（write/fsync 走 io_uring）
```

---

## 3. 推荐阅读顺序（由里到外）

1. **先懂闸门**：`snapshot_gate.h` → `command_router.cpp:32` / `shard_worker.cpp:70`（为什么 fork 前要排空）
2. **再懂子进程**：`aof_rewrite.cpp` 的 `RewriteAofChild` + `EmitKey`（无分配编码器）
3. **再懂 buffer 与换 fd**：`aof_writer.cpp` 的 `BeginRewrite` / `Append:112` / `CompleteRewrite`
4. **再懂协调**：`persistence_manager.cpp` 的 `StartRewrite` / `OnChildExit`
5. **再看收割**：`server.cpp` 的 `ChildSignalHandler` / `HandleChildExit`
6. **再看 RDB**：`rdb_writer.cpp` / `rdb_loader.cpp`（SAVE 不 fork、BGSAVE fork 的取舍）
7. **再看 IOEngine**：`io_engine.h` → `posix_io.cpp` → `uring_io.cpp`
8. **再看命令入口**：`server_commands.cpp` 的三个新命令
9. **最后验证**：`tests/unit/week6_test.cpp` + `scripts/{rewrite_test,rdb_test}.sh`

---

## 4. 关键代码逐段讲解

### 4.1 快照闸门 — `snapshot_gate.h:16`

```cpp
class SnapshotGate {
  void LockShared()    { mu_.lock_shared(); }   // 命令任务/定时器：并发
  void UnlockShared()  { mu_.unlock_shared(); }
  void LockExclusive() { mu_.lock(); }          // fork 前：排他
  void UnlockExclusive(){ mu_.unlock(); }
  std::shared_mutex mu_;
};
```

两个持 shared 锁的地方：`command_router.cpp:32`（每个路由到 shard 的命令任务）和
`shard_worker.cpp:70`（每个 shard 的定时器：过期/淘汰/rehash）。

> **为什么需要它**：`fork()` 只把调用它的那一条线程带进子进程，子进程看到的是 fork 那一
> 瞬间的**内存快照**。如果某个 shard worker 此刻正把 `Dict` 改到一半（rehash 搬迁、桶链
> `next` 指针半更新），子进程就会读到"撕碎的中间态"——这不是 C++ 数据竞争（跨进程 COW），
> 而是逻辑上不一致的脏快照。exclusive 锁保证 fork 时没有任何 shard 任务在途，快照一致。
>
> **为什么是 `shared_mutex` 而不是普通 `mutex`**：普通 mutex 会把所有 shard 命令**串行化**，
> 直接废掉"分片无锁"的并发收益。shared 锁之间互不排斥，正常读写仍完全并发，只有极低频的
> fork 才短暂排他。代价是每个 shard 命令多一次原子操作（`lock_shared` ≈ 一次原子 RMW）。

### 4.2 无分配子进程编码器 — `aof_rewrite.cpp`

核心函数 `RewriteAofChild`（`aof_rewrite.cpp:234`）：

```cpp
void RewriteAofChild(CommandRouter* router, const char* temp_path,
                     char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap) {
  int fd = ::open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) _exit(1);
  OutBuf out{fd, out_buf, out_cap, 0};
  RecBuf rec{rec_buf, rec_cap, 0};
  EmitCtx ctx{&rec, &out, false};
  if (!OutAppend(&out, aof::kMagic, aof::kMagicSize)) _exit(1);  // 文件头
  for (size_t i = 0; i < router->shard_count(); ++i)              // 遍历每个 shard
    router->ShardAt(i)->ScanSnapshot(EmitKey, &ctx);              // 无分配遍历
  if (!ctx.failed && !Flush(&out)) ctx.failed = true;
  if (ctx.failed) { ::close(fd); _exit(1); }
  ::fsync(fd); ::close(fd);
  _exit(0);
}
```

> **为什么子进程必须无分配**：多线程进程 `fork()` 后，子进程里只剩调用它的那条线程，其余
> 线程（8 个 shard worker + 4 个 IO reactor + everysec 线程）原地消失。若它们中某条在 fork
> 瞬间正持有 glibc malloc 的 arena 锁，那个锁在子进程里**永远没人释放**，子进程一 `malloc`
> 就永久死锁。所以子进程里**不调用任何会 malloc/加锁的东西**：`std::string`、`LOG_*`、
> `EncodeCommand` 统统不行，只能用预分配的 `out_buf`/`rec_buf` + `::open/::write/::fsync`。
>
> **为什么是 `_exit` 不是 `exit`**：`exit` 会跑 `atexit` 注册的清理函数、flush `stdio` 缓冲，
> 这些内部都可能 malloc/加锁；`_exit` 是纯 syscall，直接终止。子进程做完事只能 `_exit`。
>
> 遍历靠 `Shard::ScanSnapshot`（`shard.cpp:477`）——无锁、无分配地走 `dict_` 两张表
> （rehash 期间条目分散两表），并对每个条目回调 `(key, key_len, Object*, ttl_ms)`。

编码器把每个 key 翻译成重放器认识的命令（`EmitKey`，`aof_rewrite.cpp:213`）：
- string → `SET key value`（`EmitSet`，`aof_rewrite.cpp:149`）；int 编码用 `I64toa` 写成十进制，string 编码直接读 sds。
- hash → 每字段一条 `HSET key field value`（`EmitHash`，`aof_rewrite.cpp:157`）。
- list → 每 128 个元素一条 `RPUSH key e1 e2 ...`（`EmitList`，`aof_rewrite.cpp:180`）。
- TTL → `PEXPIREAT key <绝对 ms>`（`EmitPexpireat`，`aof_rewrite.cpp:203`）。

> **为什么 list 要拆成多条 RPUSH**：单条记录会先堆在 `rec` 缓冲里算 CRC，而 `rec` 是 fork 前
> 预分配的固定大小（1 MiB）。一个超大 list 的 `RPUSH` 会超出缓冲；按 128 个元素分块，每块
> 独立成一条记录，`RPUSH` 追加到队尾，顺序不变，也把单条记录大小限制住。代价是恢复时多条
> `RPUSH` 而不是一条——语义等价。

### 4.3 rewrite buffer 与换 fd — `aof_writer.cpp`

`Append`（`aof_writer.cpp:108`）多了一行：

```cpp
void AofWriter::Append(const std::string& record) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) return;
  if (!WriteAll(record.data(), record.size())) return;
  if (rewriting_) rewrite_buffer_.append(record);  // rewrite 期间并行缓冲
  ...
}
```

`BeginRewrite`（`aof_writer.cpp:147`）置 `rewriting_=true` 并清空 buffer；`CompleteRewrite`
（`aof_writer.cpp:153`）在**同一把 `mu_` 下**完成合并 + 换 fd：

```cpp
bool AofWriter::CompleteRewrite(const std::string& temp_path) {
  std::lock_guard<std::mutex> lock(mu_);         // 全程持锁，Append 插不进来
  int tmp = io_->Open(temp_path, O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  io_->Write(tmp, rewrite_buffer_.data(), rewrite_buffer_.size());  // 补写 fork 期间的写入
  io_->Fsync(tmp); io_->Close(tmp);
  ::rename(temp_path, path_);                    // 原子替换
  io_->Close(fd_);
  fd_ = io_->Open(path_, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);  // fd_ 指向新文件
  rewrite_buffer_.clear(); rewriting_ = false;
}
```

> **为什么 `BeginRewrite` 必须在 fork 之前、持 exclusive 闸门时调用**（见
> `persistence_manager.cpp:105-106`）：子进程的快照是 fork 那一瞬间的状态。fork 之后的所有
> 写入必须进 buffer、fork 之前的所有写入必须**已经**在快照里。如果在 fork 之后才
> `BeginRewrite`，中间就有一个窗口——某条写入既不在快照（fork 已过）也不在 buffer（
> `rewriting_` 还没置真）——换文件后这条写入就丢了。先排空、再置 `rewriting_`、再 fork，
> 三者无缝衔接，buffer 恰好只装 fork 之后的写入。
>
> **为什么 `CompleteRewrite` 全程持 `mu_`**：`Append` 也持同一把 `mu_`。如果"写 buffer 到
> temp → rename → 换 fd"拆成多段锁，中间某个 `Append` 就可能把一条记录写进旧 `fd_`（旧的
> 已 unlink 的 inode）、而这条记录又没进 buffer（buffer 已经清空）——丢失。全程持锁则没有
> 这个缝隙。
>
> **为什么失败时绝不做 rename**：`AbortRewrite`（`aof_writer.cpp:195`）只清 buffer + 复位
> `rewriting_`，旧日志原样保留——因为 rewrite 期间每条写入**同时也写进了旧 AOF**（`Append`
> 的 `WriteAll` 那行），旧日志本身就是完整的，弃掉 temp 即可。

### 4.4 协调与收割 — `persistence_manager.cpp` + `server.cpp`

`StartRewrite`（`persistence_manager.cpp:83`）先做三件守卫（AOF 未开 / 已有子进程 / 已在
rewrite），然后：

```cpp
auto& gate = GlobalSnapshotGate();
gate.LockExclusive();
aof_->BeginRewrite();               // 关键：fork 前、排空后置位
pid_t pid = ::fork();
if (pid == 0) RewriteAofChild(...); // 子进程永不返回
gate.UnlockExclusive();
child_pid_ = pid; child_kind_ = 1;  // 父进程记录，等待 SIGCHLD
```

SIGCHLD 收割（`server.cpp:45` + `server.cpp:158`）复用了 Week 4 已有的 eventfd 模式：

```cpp
void ChildSignalHandler(int /*sig*/) {         // 信号处理器：只写 eventfd（async-signal-safe）
  uint64_t one = 1; if (g_child_fd >= 0) ::write(g_child_fd, &one, sizeof one);
}
void Server::HandleChildExit() {               // 主循环里真正 waitpid
  int status; pid_t pid;
  while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) pm_.OnChildExit(pid, status);
}
```

`OnChildExit`（`persistence_manager.cpp:203`）按 `child_kind_` 分发：rewrite 走
`CompleteRewrite`，bgsave 走 `FinishBgSave`；子进程非零退出则 `AbortRewrite` + `unlink` temp。

> **为什么信号处理器里只写 eventfd 而不直接 waitpid**：`signal handler` 里只能调用
> async-signal-safe 函数，`waitpid` 恰好是，但为了和已有的 SIGINT/SIGTERM 关停流程统一、
> 也为了把"子进程退出后要做的一系列收尾"放到主循环线程上串行执行（收尾要拿 `mu_`、要
> rename），信号处理器只发个字节，真正的活交给 `Channel` 回调。

### 4.5 RDB — `rdb_writer.cpp` / `rdb_loader.cpp`

格式（`rdb_writer.h:20`）：`[magic MYRDB001][version 0x01]` + 逐 key 一条 `[len][crc][payload]`
记录 + `[EOF 0xFF]`。payload 里 `[type][has_ttl][ttl?][key_len][key][value]`，value 按
string/hash/list 分形。CRC 复用 `aof::Crc32`。

写入 `RdbWriteSnapshot`（`rdb_writer.cpp:212`）与 rewrite 子进程共用同一套无分配套路（
`OutBuf`/`RecBuf`/`FinishRecord`），差异只在把 key 序列化成二进制而非 RESP。

**SAVE 不 fork、BGSAVE fork**（`persistence_manager.cpp:130` vs `:161`）——这是本文件最值得
记住的取舍：

```cpp
// SAVE：持闸门 exclusive，调用线程单线程遍历写
gate.LockExclusive();
RdbWriteSnapshot(router_, tmp, ...);   // 无 worker 在改，安全，且可自由分配
gate.UnlockExclusive();
::rename(tmp, rdb_path_);

// BGSAVE：fork 子进程写，父进程立即返回
gate.LockExclusive();
pid = ::fork();
if (pid == 0) WriteRdbChild(...);      // 子进程无分配
gate.UnlockExclusive();
```

> **为什么 SAVE 不 fork**：SAVE 的语义就是"阻塞、全库快照"。既然要阻塞，不如直接持 exclusive
> 闸门排空 worker，在调用线程单线程遍历——此时没有 worker 并发改 shard，安全，还能**自由
> 分配**（不用写那套无分配编码器）。代价就是 SAVE 期间所有写被闸门挡住（阻塞，符合语义）。
> **为什么 BGSAVE 要 fork**：BGSAVE 要求不阻塞写入，必须把"读快照"挪到子进程，靠 COW 在
> 后台并行，父进程立即返回。代价就是子进程只能无分配。二者一个省心（SAVE）、一个不阻塞
> （BGSAVE），各牺牲一样。
>
> **为什么都写 temp + rename 而不是直接写 `dump.rdb`**：直接写可能在写到一半时崩溃，留下
> 一个半截的 dump.rdb 把上次的好快照也毁了。先写 temp、`fsync`、再 `rename`（原子），旧
> 快照要么完整保留、要么被完整替换，永不出现半截状态。

加载 `RdbLoad`（`rdb_loader.cpp:121`）把文件整段读进内存，逐条校验 CRC、解析、按 key 哈希
路由到 `ShardAt` 落库（`ApplyRecord`）。它在启动阶段、worker 启动前单线程执行，可自由分配。

### 4.6 IOEngine — `io_engine.h` / `posix_io.cpp` / `uring_io.cpp`

接口（`io_engine.h:15`）只抽象热数据路径 `Open/Write/Pread/Fsync/Ftruncate/Close`；
`rename` 不是 io_uring 热点，仍由调用方用普通 syscall。工厂 `CreateIOEngine` 按名字建引擎，
io_uring 未编译或初始化失败时回退 posix 并告警。

`PosixIO` 就是把原来 `AofWriter` 里的裸 syscall 原样搬过去（`posix_io.cpp`）。`UringIO`
（`uring_io.cpp`）把 `Write`/`Fsync` 换成 `io_uring_prep_write/fsync` + `submit` + `wait_cqe`，
其余复用 `PosixIO`。

> **为什么同步 submit/wait 封装的 io_uring 不会比 posix `write()` 快**：普通 `write()` 一次
> 系统调用就完成了"进页缓存"。io_uring 的同步用法要 `io_uring_submit`（进 SQ）+
> `io_uring_wait_cqe`（等完成）至少两次系统调用，单条上反而更慢。io_uring 真正的收益是**批
> 提交**：把多条 write、或 write+fsync 绑成 linked SQE 一次 `io_uring_enter` 提交，省下大量
> syscall 往返。所以本实现是"把路径打通 + 压测对比"，异步批提交是后续工作（`io_bench.cpp`
> 注释里也如实写了这一点）。

### 4.7 CRC 表的 constexpr 化 — `aof_format.cpp:15`

```cpp
constexpr std::array<uint32_t, 256> MakeCrcTable() { ... }        // 编译期算好
uint32_t Crc32(const void* data, size_t len) {
  static constexpr std::array<uint32_t, 256> table = MakeCrcTable();  // 常量初始化
  ...
}
```

原来写的是 `static const auto table = []{...}();`——一个**函数级 static**，C++11 起带线程安全
的惰性初始化 guard（magic static）。如果在 fork 瞬间恰好有线程正在第一次初始化这张表，子进程
里的 guard 就是"锁定中"状态且无人释放，子进程第一次调 `Crc32` 会**永久阻塞**。改成
`constexpr` 后表在编译期/加载期完成，无 guard、无惰性，子进程调用绝对安全。

### 4.8 list 重放修复 — `aof_recovery.cpp:211`

Week 5 加了 list，`list_commands.cpp` 往 AOF 记 `LPUSH/RPUSH/LPOP/RPOP`，但重放器
`ApplyAofCommand` 当时没同步补上——开 AOF 时 LPUSH 后崩溃重启会直接报
"cannot replay AOF command 'lpush'" 拒绝启动。这周补上了四种（`aof_recovery.cpp:211-223`）。

> 这跟 Week 5 的"`INCR` 记成 `SET key <结果>` 而不是 `INCR`"是**同一个取舍**：AOF 记的是
> 能被重放器幂等重建的命令。`SET`/`HSET`/`PEXPIREAT` 是"绝对结果"（幂等），`RPUSH` 是结构
> 增量操作——重放器必须真正理解它才能恢复 list。这也是 rewrite 子进程为什么只 emit 这几个
> 重放器认识命令的原因（见 4.2）。

---

## 5. 自查题（先自己想，再对答案）

### Q1 — fork 语义
`fork()` 之后子进程里有几条线程？为什么会有"只剩调用线程"这种看似奇怪的语义？

### Q2 — malloc 死锁
为什么多线程进程 fork 后，子进程里 `malloc` 可能永久死锁？我们用了哪三招规避？

### Q3 — _exit vs exit
子进程结束时为什么用 `_exit` 而不是 `exit`？

### Q4 — 闸门类型
快照闸门为什么用 `shared_mutex` 而不是普通 `mutex`？代价是什么？

### Q5 — BeginRewrite 时机
为什么 `BeginRewrite()` 必须在 fork 之前、持 exclusive 闸门时调用？如果挪到 fork 之后会出什么 bug？

### Q6 — 换 fd 的原子性
`CompleteRewrite` 为什么从写 buffer 到 rename 到换 fd 要全程持 `mu_`？拆开会丢哪条写？

### Q7 — CRC 表
CRC 表为什么要从函数级 `static` 改成 `constexpr`？（提示：magic static + fork）

### Q8 — list 分块
rewrite 出的 AOF 为什么把 list 拆成每 128 个元素一条 `RPUSH`，而不是一条塞满？

### Q9 — SAVE vs BGSAVE
`SAVE` 为什么不 fork、`BGSAVE` 为什么 fork？各牺牲了什么？

### Q10 — temp + rename
RDB（以及 rewrite）为什么先写 temp 再 `rename`，而不是直接写目标文件？

### Q11 — io_uring
同步 submit/wait 封装的 io_uring 为什么不会比 posix `write()` 快？真正收益在哪？

### Q12 — 幂等性
启动时 AOF 和 RDB 谁优先？顺带修的 list 重放 bug 和 Week 5 的"INCR 记成 SET"是同一个什么取舍？

---

## 6. 答案

### A1
子进程里**只有一条**：调用 `fork()` 的那条线程。因为 POSIX `fork` 只复制"当前线程"到新
进程，其它线程随原进程继续存活在父进程里、在子进程里不存在。这带来两个后果：其一，子进程
看到的是一份 COW 快照，能安全地读父进程内存；其二，其它线程持有的任何锁/资源在子进程里
"没人会释放"，所以子进程只能做 async-signal-safe 的事。

### A2
glibc 的 `malloc` 有 per-arena 锁。若 fork 瞬间某条线程正持着某个 arena 的锁，这个锁在子进程
里随该线程一起"消失"、永远锁着；子进程再 `malloc` 就死锁。规避三招：(1) 子进程全程**无分配**
（`std::string`/`vector`/`LOG` 都不碰）；(2) 用 **fork 前预分配**的 `out_buf`/`rec_buf`；(3)
结束用 `_exit` 而非 `exit`（`exit` 会 flush stdio、跑 atexit，可能 malloc）。

### A3
`exit` 会执行 `atexit` 注册的清理函数并 flush `stdio` 缓冲，这些内部可能 malloc/加锁，在
"只剩一条线程、可能还有半截锁"的子进程里是危险的。`_exit` 是纯系统调用、直接终止，不触发
任何用户态清理。

### A4
普通 `mutex` 会让所有 shard 命令串行化，直接废掉"分片无锁"的并发收益。`shared_mutex` 的
shared 锁之间互不排斥——正常读写仍然完全并发，只有极低频的 fork 才用 exclusive 锁排他。
代价是每个 shard 命令多一次原子操作（`lock_shared` ≈ 一次原子 RMW）。

### A5
fork 的快照是"fork 那一刻"的状态。buffer 必须恰好装 fork 之后的写入：fork 之前的所有写入要
已经在快照里，fork 之后的所有写入要进 buffer。如果 fork 之后才 `BeginRewrite`，就有一个窗口
——某条写入既不在快照（fork 已过）也不在 buffer（`rewriting_` 还没置真），换文件后丢写。
先排空（exclusive）→ 置 `rewriting_` → fork，三者无缝衔接。

### A6
`Append` 也持 `mu_`。若"写 buffer → rename → 换 fd"拆成多段锁，中间某个 `Append` 会把一条
记录写进旧 `fd_`（已 unlink 的 inode）而它又不在 buffer 里（buffer 已清空）——丢写。全程持锁
则没有这个缝隙，任何时刻都处于"要么旧 fd 还能接写、要么新 fd 已就位且 buffer 已合并"的一致态。

### A7
原来的 `static const auto table = []{...}();` 是函数级 static，C++11 起带线程安全的惰性初始化
guard（magic static）。若 fork 瞬间恰好有线程在首次初始化这张表，子进程里的 guard 是"锁定中"
且无人释放，子进程第一次 `Crc32` 永久阻塞。改成 `constexpr` 后表在编译期/加载期完成，无 guard、
无惰性，子进程调用绝对安全。

### A8
单条记录会先堆在 `rec` 缓冲里算 CRC，而 `rec` 是 fork 前预分配的固定大小（1 MiB）。一个超大
list 的一条 `RPUSH` 会撑爆缓冲导致 rewrite 失败。按 128 个元素分块，每块独立成一条记录，
`RPUSH` 追加到队尾、顺序不变，单条记录大小也被限制住。代价是恢复时多条 `RPUSH` 而非一条，
语义等价。

### A9
SAVE 语义是"阻塞全库快照"，既然要阻塞，直接持 exclusive 闸门排空 worker、在调用线程单线程
遍历即可——此时无并发，安全，还能自由分配（省掉无分配编码器），牺牲的是 SAVE 期间所有写被
挡住。BGSAVE 要求不阻塞写，必须 fork 把"读快照"挪到子进程靠 COW 后台并行，父进程立即返回，
牺牲的是子进程只能无分配。

### A10
直接写目标文件可能在写到一半崩溃，留下半截文件把上次的好快照也毁了。先写 temp、`fsync`、
再 `rename`（原子），目标文件要么完整保留旧内容、要么被完整替换，永不出现半截状态。

### A11
普通 `write()` 一次系统调用就完成了"进页缓存"。io_uring 的同步用法要 `submit` + `wait_cqe`
至少两次系统调用，单条反而更慢。io_uring 真正收益是**批提交**：多条 write 或 write+fsync 绑
成 linked SQE 一次 `io_uring_enter` 提交，省下大量 syscall 往返。本实现是打通路径 + 压测对比，
异步批提交是后续工作。

### A12
AOF 优先（`persistence_manager.cpp:71`：`appendonly ? ReplayAndOpen : LoadRdbIfPresent`），
因为 AOF 是逐条命令日志、恢复后最新，RDB 只是某个时间点的快照。list 重放 bug 和"INCR 记成
SET"是同一个**幂等性取舍**：AOF 只记重放器能确定重建的命令。`SET`/`HSET`/`PEXPIREAT` 是绝对
结果（幂等），`RPUSH`/`LPOP` 是结构增量操作，重放器必须真正理解它们——Week 5 加了 list 却漏
了补重放器，这周补上。

---

## 7. 快速自测对照表

| 如果你能答对… | 说明你理解了… |
|---------------|---------------|
| Q1、Q2、Q3 | fork 在多线程进程里的安全模型（最核心） |
| Q4、Q5 | 快照一致性与"无缝隙衔接" |
| Q6 | 换 fd 的原子性 / 丢写窗口 |
| Q7 | magic static + fork 的隐蔽死锁 |
| Q8 | 固定缓冲下的大对象序列化取舍 |
| Q9、Q10 | RDB 的同步/异步 + 崩溃安全的文件替换 |
| Q11 | io_uring 的收益模型（批提交 vs 同步） |
| Q12 | AOF 的幂等性设计原则 + AOF/RDB 优先级 |

---

## 8. 达标标准（读到什么程度才算可以）

分四级，**「可以」的线在 L2**：

- **L0 没读懂**：只能说出"有 `BGREWRITEAOF`/`SAVE`/`BGSAVE` 命令、会 fork"。这不算读懂。
- **L1 能跟读**：对着代码能说清三条数据流（rewrite / RDB / IOEngine）、每个文件负责什么。
  也就是做完第 1、2、3 节。
- **L2 可以（面试线）**：**合上代码**能独立复述 fork 那一刻的全流程——排空闸门 → `BeginRewrite`
  → `fork` → 子进程无分配遍历写 temp → `_exit` → SIGCHLD → `waitpid` → 合并 buffer → `rename`
  → 换 fd——并且答对第 5 节 ≥ 10/12 题，重点是每个"**为什么 + 代价**"：闸门为什么要有、子进程
  为什么不能 malloc、`_exit` vs `exit`、buffer 为什么在 fork 前开始、rename 为什么全程持锁、
  SAVE 为什么不 fork、同步 io_uring 为什么没收益。
- **L3 很好**：能讲出"删掉某一环会出什么具体 bug"——删闸门 → fork 读到撕碎的 dict；`exit` 替
  `_exit` → 子进程跑 atexit 崩；buffer 在 fork 后才 Begin → 丢写窗口；SAVE 改 fork 也行但多了
  无谓开销；`rename` 拆锁 → 换 fd 时丢写。

**判据**：L2 是"可以"的最低线。自查题答对 ≥ 10/12，且能不看代码复述 fork 流程 = 达到 L2。
一条可操作的自测：跑 `bash scripts/rewrite_test.sh`，对着日志里的 `AOF rewrite started` /
`AOF rewrite completed` 两行，把 fork 前后每一步讲出来——讲得通，就达标。
