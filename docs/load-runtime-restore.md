# 运行时还原（LOAD 命令，DEBUG RELOAD 风格）

`LOAD` 命令：在服务已运行、shard 已有数据的情况下，从 RDB 文件整库重载，覆盖当前内存数据。
这是对「启动还原」的补全——启动还原只在 `Server::Start()` 里、shard 还空着、单线程时发生一次
（`src/server/server.cpp:97` 的 `pm_.Init`），而 `LOAD` 面对的是「shard 已有数据 + worker 线程
正在跑」的运行时场景，必须正面解决启动还原被绕过的那三个难点。

## 三个难点与处理方式

1. **并发** — 复用 `GlobalSnapshotGate`（`include/mycache/persistence/snapshot_gate.h`）。
   命令任务（`command_router.cpp:36-39`）与定时器任务（`shard_worker.cpp:70-76`）执行期间都持
   shared 锁；`LOAD` 全程持 exclusive 锁（RAII 守卫 `ExclusiveLock()`），排空在途任务后清空 + 加载。
   这与 `Save()`（`persistence_manager.cpp` 的 `Save`）的阻塞式快照模式一致。

2. **原子性** — 采用「先 flush 再 load」的非原子方案：`Shard::FlushAll()` 清空后 `RdbLoad` 重载，
   失败后 DB 为空，不保证跨 shard 原子（与 Redis `DEBUG RELOAD` 一致）。

3. **AOF 一致性** — appendonly 开启时**直接拒绝** LOAD，避免「内存与 AOF 日志失步」。

## 关键代码位置

- `include/mycache/persistence/snapshot_gate.h` — `ExclusiveLock()` RAII 守卫。
- `include/mycache/storage/db/shard.h` + `src/storage/db/shard.cpp` — `Shard::FlushAll()`。
- `include/mycache/persistence/persistence_manager.h` + `.cpp` — `PersistenceManager::Load()`。
- `src/command/server_commands.cpp` — `LoadCommand`（`LOAD` 命令注册）。

## 失败后的状态

- 加载中途失败（如 RDB 文件 CRC 错在中间）会**清空 DB**，错误信息带 `(database was flushed)`，
  不做回滚。
- 无 RDB 文件、appendonly 开启这两类失败发生在 flush **之前**，不会清空数据。

## 面试话术（三段）

### 1. 为什么选择非原子方案

「启动时加载 RDB，文件坏了大不了拒绝启动，进程起不来、用户立刻知道。但运行时重载不一样，文件可能
坏在中间，而我已经把旧数据清空了。我选择的是和 Redis `DEBUG RELOAD` 一样的语义——先 `emptyDb` 清库、
再 `rdbLoad`，加载失败就把 DB 留在空状态、不做回滚。代价是失败会丢数据，换来的是实现简单、语义明确、
没有隐藏中间态。真想原子，得先把整个文件读进临时结构、逐条验 CRC 全通过再一次性 swap；而这个项目是
分片 dict，没有『整体 swap』原语，要上就得逐 shard 加锁或双缓冲，复杂度上一个台阶，所以我把它列为
后续扩展而不是这一版就做。」

### 2. 为什么 appendonly 开启时拒绝 LOAD，而不是自动 AOF rewrite

「AOF 开着时我重载了内存，但 AOF 文件里还躺着旧的写历史，内存和日志就失步了——下次重启重放 AOF 会
得到『旧历史 + 新加载』的错乱。要修得在重载成功后触发一次 AOF rewrite，把当前内存态重新写成新日志。
但 rewrite 是 fork 子进程异步完成的，从 LOAD 完成到 rewrite 落盘之间有一个崩溃窗口，这段时间挂了日志
还是旧的。所以精简版里我选择直接拒绝：appendonly 开启时 LOAD 返回明确错误。这不是回避问题，而是
『宁愿拒绝、不留失步』——一致性优先于便利。」

### 3. 为什么不实现正式 LOAD，只做 DEBUG RELOAD 风格

「Redis 本来就没有正式的 `LOAD` 命令，最接近的是 debug-only 的 `DEBUG RELOAD`，它内部就是 emptyDb
后 rdbLoad。所以我把这个功能定位成 DEBUG RELOAD 风格——一个阻塞式、不保证原子、需要操作者自己担责的
重载命令，而不是面向普通客户端的正式数据迁移接口。真要做正式 LOAD，得考虑文件名参数、权限、原子性、
AOF 联动，那已经超出『持久化恢复』这一章的练习范围了。」

## 后续扩展（明确不做）

1. **原子性升级**：两遍校验——先全量读入 + 验 CRC 再一次性 swap。
2. **AOF 联动**：重载成功后触发 `StartRewrite` 重写 AOF（仍有崩溃窗口）。
3. **分片级锁 / 双缓冲**：缓解整库 exclusive 锁长时间阻塞。
4. **文件名参数**：当前 `arity=1` 不支持 `LOAD <filename>`；支持指定文件需改 arity 为 `-1` 并解析
   `args[1]` 作为路径。
