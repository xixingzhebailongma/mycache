# Week 4 代码阅读指南：AOF 持久化与崩溃恢复

本文是配套 `docs/persistence.md`（讲"是什么/为什么"）的**代码阅读地图**，讲的是
"代码在哪、按什么顺序读、每段在干什么"。读完并做完末尾的自查题，就能真正掌握这一周
的改动。

---

## 1. 改动清单

### 新增（核心）
| 文件 | 作用 |
|------|------|
| `include/mycache/persistence/aof/aof_format.h` | 文件头 magic、记录头结构、`Crc32`/`EncodeCommand`/`EncodeRecord`/`LogCommand` 声明 |
| `src/persistence/aof/aof_format.cpp` | 上面这些函数的实现（CRC32、RESP 编码、打包） |
| `include/mycache/persistence/aof/aof_writer.h` | `AofWriter`（三种 fsync 策略）、`AofFsync` 枚举、`ParseAofFsync` |
| `src/persistence/aof/aof_writer.cpp` | 打开/关闭/追加/后台 fsync 线程 |
| `include/mycache/persistence/aof/aof_recovery.h` | `AofRecovery::Replay`、`ApplyAofCommand`、结果结构 |
| `src/persistence/aof/aof_recovery.cpp` | 读文件、校验、重放、截断修复、命令应用到 shard |

### 新增（测试/工具）
| 文件 | 作用 |
|------|------|
| `tests/unit/aof_test.cpp` | 12 个 gtest：格式、往返、TTL、半条记录、checksum、策略解析 |
| `benchmark/aof_bench.cpp` | 三种策略吞吐对比 |
| `scripts/crash_test.sh` | kill -9 后重启不丢数据（三种策略循环） |
| `scripts/recovery_test.sh` | 优雅重启 + 尾部半条记录修复 |

### 修改
| 文件 | 改动 |
|------|------|
| `include/mycache/common/time.h` | 新增 `UnixMs()`（墙钟） |
| `include/mycache/common/config.h` | 新增 `appendonly`/`appendfilename`/`appendfsync`/`dir` + `AofPath()` |
| `include/mycache/storage/db/shard.h` / `.cpp` | 新增 `ExpireAt`，到期时间改用 `UnixMs()` |
| `include/mycache/command/command_context.h` | 新增 `AofWriter* aof` 指针 |
| `src/command/string_commands.cpp` | `SET`/`DEL` 打日志 |
| `src/command/generic_commands.cpp` | `EXPIRE`/`PEXPIRE`→`PEXPIREAT` 打日志 |
| `src/command/hash_commands.cpp` | `HSET`/`HDEL` 打日志 |
| `include/mycache/server/command_router.h` | 新增 `ShardAt(i)` 直接访问 shard |
| `include/mycache/server/server.h` / `.cpp` | 持有 `AofWriter`、`InitAof()`、启动时恢复、分发时传 aof、停止时关闭 |
| `CMakeLists.txt` | 挂载 `aof_test.cpp`、新增 `mycache_aof_bench` |

---

## 2. 一张图看懂两条数据流

### 写入路径（运行时）
```
客户端 SET k v
   └─► IO 线程解析 RESP ──► DispatchCommand（server.cpp:216）
          └─► 按 key 哈希路由到 ShardWorker 线程
                └─► handler(ctx, args)   // ctx.db = 该 shard，ctx.aof = 全局 AofWriter
                      ├─► ctx.db->Set(k, v)            // 改内存
                      ├─► aof::LogCommand(ctx.aof, ...) // 追加到 AOF
                      └─► ctx.ReplySimple("OK")         // 回复客户端
```

### 恢复路径（启动时，shard worker 启动之前）
```
Server::Start()
   └─► InitAof()（server.cpp:107）
         ├─► AofRecovery::Replay(path, apply)  // 逐条读 + 校验 + 重放 + 修尾巴
         │      └─► apply = 按 args[0] 哈希 → router_.ShardAt(idx) → ApplyAofCommand
         └─► aof_->Open(path, fsync)           // 打开追加写
```

---

## 3. 推荐阅读顺序（由里到外）

1. **先懂格式**：`aof_format.h` → `aof_format.cpp`（记录到底长什么样）
2. **再懂写**：`aof_writer.h` → `aof_writer.cpp`（怎么落盘、三种策略、后台线程）
3. **再懂读**：`aof_recovery.h` → `aof_recovery.cpp`（怎么重放 + 修复）
4. **再看触发点**：`string_commands.cpp` / `generic_commands.cpp` / `hash_commands.cpp`（哪些命令、何时打日志）
5. **再看串起来**：`server.cpp` 的 `InitAof()` 和 `DispatchCommand`
6. **最后看 TTL 改动**：`time.h` 的 `UnixMs()` + `shard.cpp` 的 `ExpireAt`
7. **验证**：`tests/unit/aof_test.cpp` + 两个 shell 脚本

---

## 4. 关键代码逐段讲解

### 4.1 记录格式 — `aof_format.h:17` / `aof_format.cpp`

```cpp
// aof_format.h
inline constexpr char kMagic[] = "MYAOF001";   // 8 字节文件头
constexpr size_t kMagicSize = 8;

struct RecordHeader {
  uint32_t len = 0;   // payload 字节数（小端）
  uint32_t crc = 0;   // payload 的 CRC32（小端）
};
```

一条记录 = `[8 字节头][payload]`，其中 payload 是一条 RESP 数组命令。`EncodeRecord`：

```cpp
std::string EncodeRecord(const std::string& payload) {
  RecordHeader h;
  h.len = (uint32_t)payload.size();
  h.crc = Crc32(payload.data(), payload.size());
  std::string out;
  out.reserve(kMagicSize + payload.size());
  for (int i = 0; i < 4; ++i) out.push_back((char)((h.len >> (8*i)) & 0xFF)); // 小端写 len
  for (int i = 0; i < 4; ++i) out.push_back((char)((h.crc >> (8*i)) & 0xFF)); // 小端写 crc
  out += payload;
  return out;
}
```

> 为什么用 `[len][crc]` 而不是像 Redis 那样纯 RESP？因为有了长度和校验和，
> 恢复时才能**确定性地**判断"这一条记录写完整了没有"（见 4.3）。

### 4.2 写入器 — `aof_writer.cpp`

**打开（`Open`，行 22）** 用 `O_RDWR | O_CREAT | O_APPEND`：
- 空文件 → 写 magic；
- 文件头被截断（0 < size < 8）→ 截断重写；
- 已有完整文件 → `pread` 回读前 8 字节校验 magic 对不对。

```cpp
fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
```

> 注意是 `O_RDWR` 而不是 `O_WRONLY`——后面要用 `pread` 回读文件头，写只打开的文件
> 无法 `pread`（见末尾 Q10）。

**追加（`Append`，行 99）** —— 核心，三种策略的分岔点就在这里：

```cpp
void AofWriter::Append(const std::string& record) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fd_ < 0) return;
  if (!WriteAll(record.data(), record.size())) return;  // 每次都 write 到页缓存
  if (policy_ == AofFsync::kAlways) {
    Fsync();                  // 每次都 fsync
  } else if (policy_ == AofFsync::kEverysec) {
    dirty_ = true;            // 标记"有脏数据"，交给后台线程
  }
  // kNo：啥也不干，交给 OS
}
```

**后台线程（`FlushLoop`，行 135）** —— 只有 everysec 会启动：

```cpp
void AofWriter::FlushLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (running_) {
    bool stop = cv_.wait_for(lock, kFlushInterval, [this]{ return !running_; });
    if (stop) break;
    if (dirty_) { dirty_ = false; if (fd_ >= 0) ::fsync(fd_); }
  }
}
```

### 4.3 恢复器 — `aof_recovery.cpp`

整段读入内存后，用一个 `pos` 指针扫过去：

```cpp
// aof_recovery.cpp:107 起（节选）
while (pos + sizeof(aof::RecordHeader) <= size) {
  aof::RecordHeader h = DecodeHeader(buf.data() + pos);
  const size_t payload_off = pos + sizeof(aof::RecordHeader);

  // (1) 头声称的载荷长度超出文件剩余 → 半条记录（kill -9 写坏）→ 截断
  if (payload_off + h.len > size) { truncated = true; break; }

  const char* payload = buf.data() + payload_off;
  // (2) CRC 对不上
  if (aof::Crc32(payload, h.len) != h.crc) {
    if (payload_off + h.len == size) { truncated = true; break; } // 最后一条 → 截断
    r.error = "AOF checksum mismatch in middle of file";          // 中间 → 致命
    ::close(fd); return r;
  }

  // (3) 解析 RESP 命令 → apply(name, args) → 应用到对应 shard
  ...
  pos = payload_off + h.len;  // 前进到下一记录
}
if (pos < size) truncated = true;                       // 剩下不足一个完整头
if (truncated) { r.bytes_truncated = size - pos; ::ftruncate(fd, pos); } // 截断修复
```

三个关键判断，对应三种损坏：
1. `payload_off + h.len > size` —— 记录没写完整（头在、载荷缺）→ 半条记录，截掉。
2. CRC 失败且是**最后一条** —— 当作尾部损坏，截掉。
3. CRC 失败且在**中间** —— 真损坏，拒绝启动。

**命令应用（`ApplyAofCommand`，行 184）** 只处理写命令：

```cpp
if (name == "set" && args.size() == 2) { shard->Set(args[0], args[1]); return true; }
if (name == "del") { shard->Del(args); return true; }
if (name == "hset" && args.size() == 3) { shard->HSet(args[0], args[1], args[2]); return true; }
if (name == "hdel") { ... }
if (name == "pexpireat" && args.size() == 2) { ... shard->ExpireAt(args[0], when); ... }
return false;  // 未知命令 → 上层报"cannot replay"
```

### 4.4 触发点 — 写命令打日志

`string_commands.cpp:26`（SET）：

```cpp
void SetCommand(CommandContext& ctx, const std::vector<RespValue>& args) {
  ctx.db->Set(args[1].str, args[2].str);
  aof::LogCommand(ctx.aof, "SET", {args[1].str, args[2].str});  // 先改内存，再记日志
  ctx.ReplySimple("OK");
}
```

`generic_commands.cpp:37`（EXPIRE → PEXPIREAT）：

```cpp
void ExpireCommand(CommandContext& ctx, ...) {
  long long seconds = 0;
  if (!ParseIntArg(ctx, args[2], &seconds)) return;
  int64_t when = UnixMs() + seconds * 1000;             // 算一次绝对时间
  if (ctx.db->ExpireAt(args[1].str, when)) {            // 内存里存的也是这个 when
    aof::LogCommand(ctx.aof, "PEXPIREAT", {args[1].str, std::to_string(when)});
    ctx.ReplyInt(1);
  } else ctx.ReplyInt(0);
}
```

> 关键点：`when` 只算一次，内存里存的到期时间和 AOF 里记的绝对时间**完全一致**。

`DEL`（string_commands.cpp:41）和 `HDEL`（hash_commands.cpp:39）只在 `removed > 0` 时才记日志
—— 删了个不存在的 key 不该产生一条 AOF 记录。

### 4.5 串起来 — `server.cpp`

**恢复 + 打开（`InitAof`，行 107）**，在 `Start()` 里于所有线程启动前调用：

```cpp
bool Server::InitAof() {
  if (!config_.appendonly) return true;              // 没开 AOF 直接返回

  AofFsync fsync = AofFsync::kEverysec;
  if (!ParseAofFsync(config_.appendfsync, &fsync)) return false;

  const std::string path = config_.AofPath();

  auto result = AofRecovery::Replay(path, [this](name, args) {
    if (args.empty()) return false;
    size_t idx = router_.ShardIndex(args[0]);        // 按 key 哈希回同样的 shard
    return ApplyAofCommand(router_.ShardAt(idx), name, args);
  });
  if (!result.ok) { LOG_ERROR(...); return false; }  // 恢复失败 → 拒绝启动

  aof_ = std::make_unique<AofWriter>();
  if (!aof_->Open(path, fsync)) { aof_.reset(); return false; }
  return true;
}
```

**分发时把 aof 塞进 context（`DispatchCommand`，行 216）**：

```cpp
if (command->first_key > 0) {
  AofWriter* aof = aof_.get();
  router_.Route(key, [conn, handler = command->handler, args = cmd.array, aof](Shard* shard) {
    CommandContext ctx{conn, shard};
    ctx.aof = aof;            // 把全局 AofWriter 指针带给 shard 线程上的 handler
    handler(ctx, args);
  });
} else {
  CommandContext ctx{conn, nullptr};
  ctx.aof = aof_.get();
  command->handler(ctx, cmd.array);
}
```

### 4.6 TTL 为什么改墙钟 — `time.h:23` + `shard.cpp:203`

```cpp
// time.h
inline int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();  // 墙钟，重启不变
}
```

```cpp
// shard.cpp
bool Shard::ExpireAt(const std::string& key, int64_t when_ms) {
  sds k = sdsnewlen(key.data(), key.size());
  Object* o = LookupValue(k);
  if (!o) { sdsfree(k); return false; }
  expires_.Replace(k, reinterpret_cast<void*>(static_cast<intptr_t>(when_ms)));
  sdsfree(k); return true;
}
bool Shard::ExpireMs(const std::string& key, int64_t ms) {
  return ExpireAt(key, UnixMs() + ms);   // 相对 TTL 收敛到 ExpireAt
}
```

---

## 5. 自查题（先自己想，再对答案）

建议按顺序答，每题想清楚再往下看答案。

### Q1 — 格式
一条 AOF 记录由哪几部分组成？`len` 和 `crc` 各占几个字节、是什么字节序？

### Q2 — 三种策略的本质
既然三种策略每次 `Append` 都 `write()`，那 `always` / `everysec` / `no` 的区别到底在哪？
为什么 kill -9 测试里三种策略**都不丢**数据？

### Q3 — CRC 与半条记录
`Crc32` 是对哪段数据算的？如果 `kill -9` 恰好打断在一条记录的 payload 中间，恢复时
是哪一行代码、靠什么发现了这个问题？

### Q4 — 恢复时机
为什么 AOF 恢复必须赶在 shard worker 线程启动**之前**做？如果先启动线程再恢复会怎样？

### Q5 — TTL 墙钟
为什么把 TTL 的时间基准从 `NowMs()`（单调钟）换成 `UnixMs()`（墙钟）？如果保持单调钟，
重启后 `PEXPIREAT` 存的绝对时间会出什么问题？

### Q6 — EXPIRE → PEXPIREAT
写命令打日志时，为什么 `EXPIRE key 100` 要翻译成 `PEXPIREAT key <绝对时间>`，而不是
原样记一条 `EXPIRE key 100`？

### Q7 — everysec 后台线程
`FlushLoop` 是怎么工作的？`dirty_` 标志是干嘛的？为什么 fsync 要在**持锁**状态下做？

### Q8 — 并发安全
8 个 shard worker 同时写 AOF，为什么记录不会交错？这个锁会带来什么性能代价？哪种策略下
这个代价最明显？

### Q9 — 损坏分级
为什么"中间记录 CRC 失败"是致命错误（拒绝启动），而"最后一条记录 CRC 失败"却只是截断丢弃？

### Q10 — 一个真实的 bug
调试时我踩过一个坑：最初用 `O_WRONLY` 打开 AOF 文件，结果启动时 `Open` 报"unexpected
header"。你能解释为什么吗？（提示：和 `pread` 有关）

---

## 6. 答案

### A1
一条记录 = `[RecordHeader 8 字节][payload]`。`RecordHeader` 里 `len` 4 字节 + `crc` 4
字节，都是**小端**（`aof_format.cpp` 里 `(x >> (8*i)) & 0xFF` 从低字节开始写）。payload
是一条 RESP 数组命令（如 `*3\r\n$3\r\nSET\r\n...`）。文件最开头还有 8 字节 magic
`MYAOF001`。

### A2
三种策略的 `write()` 都一样（每次都进页缓存），区别只在 **fsync 的时机**：
- `always`：每条记录 `write` 后立刻 `fsync`（Append 里同步做）。
- `everysec`：`write` 后只置 `dirty_ = true`，后台线程每秒 `fsync` 一次。
- `no`：从不 `fsync`（除了优雅关闭时）。

`kill -9` 只是杀进程，数据已经 `write` 进**内核页缓存**，不会丢；所以三种策略 kill -9
都不丢。fsync 决定的是**断电/宕机**时的持久性：`always` 不丢、`everysec` 最多丢 1 秒、
`no` 可能丢最多一个 OS 写回周期（约 30 秒）。

### A3
`Crc32` 是对 **payload**（那条 RESP 命令的字节）算的，结果存在记录头里。打断在 payload
中间时，文件里这条记录的 `len` 还是完整值，但 payload 字节不足。恢复时
`aof_recovery.cpp` 里的 `if (payload_off + h.len > size)` 这一行发现"头声称的长度超出了
文件剩余字节"，判定为半条记录，`truncated = true` 截掉。（如果打断在**记录头**里，则靠
循环末尾的 `if (pos < size)` 发现"剩下不足 8 字节"，同样截掉。）

### A4
因为恢复是**单线程直接写 shard**（通过 `router_.ShardAt(idx)`），没有走 event loop，也没
加锁。如果 shard worker 线程已经在跑，恢复线程和 worker 线程会**并发访问同一个 `Shard`
对象**，产生数据竞争。所以 `InitAof()` 必须在 `router_.Start()` 之前调用（`server.cpp:55`
的 `Start()` 里，`InitAof()` 在所有线程启动之前）。

### A5
单调钟 `steady_clock` 的 epoch 是"本次开机以来的某个点"，**重启后归零**。如果用它存
`PEXPIREAT` 的绝对时间戳，重启后 `when` 里存的是上一次开机后的毫秒数，跟新进程的单调钟
完全对不上，TTL 会算出一个荒谬的剩余时间。墙钟 `system_clock`（Unix 纪元）跨重启不变，
绝对时间才有意义——这和 Redis 的 `mstime()` 用 `CLOCK_REALTIME` 是同一个道理。

### A6
因为 `EXPIRE key 100` 是**相对**的：它表示"从现在起 100 秒后过期"。如果原样记下来，重放
是在**重启后**执行的，`EXPIRE` 会以重启时刻为起点再算 100 秒，已经流逝的时间就丢了（该
过期的 key 又满血复活）。翻译成 `PEXPIREAT key <绝对毫秒>` 后，重放时不管何时执行，设置
的都是同一个绝对到期点，结果确定。

### A7
`FlushLoop` 循环里 `cv_.wait_for(lock, 1s, pred)` 每秒醒一次；醒来后若 `dirty_` 为真就
`fsync` 并清掉 `dirty_`。`dirty_` 的作用是**避免空转**：如果这一秒没有写入，就不做无谓的
fsync（省一次磁盘操作）。fsync 持锁做，是为了避免与写路径的 `write()` 交错——虽然 Linux
上并发 fsync/write 其实也安全，但持锁能让"记录先完整 write 完、再被 fsync"这个顺序清晰
无歧义。

### A8
`AofWriter::Append` 一开始就 `std::lock_guard<std::mutex> lock(mu_)`，所有 shard worker
追加时串行进入临界区，且每条记录由一次 `WriteAll` 写完整，所以不会交错。代价是写路径在
锁上串行化；`always` 最明显，因为它在**持锁状态下 fsync**（毫秒级），锁被占住期间其他
shard 的写全部排队——这就是压测里 `always` 只有 ~180 ops/s 的原因。

### A9
因为这两种情况的可信度不同：中间记录 CRC 失败意味着文件在**有效数据中间**坏了一块，前面
重放出的状态已经建立在"这条命令应该生效"的假设上，再往下重放只会更乱，必须停下来人工
处理。而**最后一条** CRC 失败几乎可以断定是 `kill -9` 写到一半留下的（我们的写路径是持锁
整条写，中间不会产生坏记录），丢掉它只损失"崩溃前最后一条"本来就没写全的命令，前面都是
好的，安全。

### A10
`pread(fd, ...)` 是一次**读**操作，而 `O_WRONLY` 打开的文件描述符**不允许读**，`pread`
会直接返回 -1 并置 `errno = EBADF`。于是 `Open` 里"回读前 8 字节校验 magic"这一步永远失败
，被误判成"文件头不对"。改成 `O_RDWR`（可读可写）后 `pread` 才能读到文件头。这个 bug 用
单元测试没抓到（单测里 `Open` 的要么是新文件要么是空文件，不走"回读校验"分支），是
`crash_test.sh` 里"重启时 Open 已有文件"这条路径暴露出来的——这也说明端到端的重启测试
不可替代。

---

## 7. 快速自测对照表

| 如果你能答对… | 说明你理解了… |
|---------------|---------------|
| Q1、Q3 | AOF 格式 + 损坏检测 |
| Q2、Q7、Q8 | fsync 策略 + 并发模型 |
| Q4 | 恢复与 shard 线程的生命周期 |
| Q5、Q6 | TTL 的持久化语义 |
| Q9 | 恢复策略的取舍 |
| Q10 | POSIX fd 权限 + 端到端测试的价值 |
