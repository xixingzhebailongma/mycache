# 性能与稳定性数据（Week 5）

环境：WSL2，单机，Release 构建（`-O3`），`redis-benchmark` 走本机 TCP。
服务端默认 `io_threads 4` / `shard_count 8`。

## 1. 吞吐与延迟（GET/SET/INCR/LPUSH）

```
redis-benchmark -t get,set,incr,lpush -n 200000 -c 50 -d 64
```

| 命令 | QPS | P99 |
|------|-----|-----|
| SET   | ~125k req/s | ~0.5 ms |
| GET   | ~130k req/s | < 1 ms（亚毫秒）|
| INCR  | ~128k req/s | < 1 ms |
| LPUSH | ~123k req/s | < 1 ms |

说明：
- 这是**深度流水线**（每个客户端一次灌入大量命令、批量回包）下的上限吞吐；单条
  round-trip（`redis-cli` 逐个发）受网络 RTT 主导，数字没有意义，所以用 benchmark 的
  流水线模式。
- 绝对数字会随机器和 WSL 抖动浮动 ~10%；**结论性的点是「亚毫秒 P99 + 12–13 万级 QPS」**，
  证明存储热路径没有锁竞争、内存分配/哈希是主要成本。
- 参照：同一台 WSL 上原生 `redis-server 7.2.5` 同参数约 20 万 QPS——本项目纯学习实现做到
  其 **~60–65%**，且热点全在无锁分片 + 内存分配，差距主要来自网络/解析层的成熟度。
- `mycache_bench`（`scripts/benchmark.sh`）是同一流水线思路的自研客户端，可用于离线
  复现。

## 2. shard 数量调优

固定 `io_threads 4`，扫 `shard_count = 1/2/4/8/16`（`-t set,get -n 100000 -c 50 -d 64`）：

| shard_count | SET QPS | GET QPS |
|-------------|---------|---------|
| 1  | ~127k | ~127k |
| 2  | ~141k | ~142k |
| 4  | ~133k | ~137k |
| 8  | ~129k | ~125k |
| 16 | ~130k | ~138k |

结论：**在 4 个 IO 线程、单个 benchmark 客户端的流水线下，吞吐对 shard 数不敏感**，
2–8 之间基本持平。原因：shard worker 早已不是瓶颈（每个 shard 无锁、串行处理自己的
key），瓶颈在 IO/网络层。所以：

- `shard_count` 的真实价值不是「提吞吐」，而是**消除跨 key 的共享锁**（Week 1 的单
  全局 `Keyspace` + `shared_mutex` 在高并发写时会退化）。shard 数 = 1 时只剩一个 worker
  串行所有命令，吞吐仍接近多 shard，恰恰说明 worker 单线程处理能力足够。
- 默认 `8` 是「够散、又不至于开太多线程」的稳妥值；`16` 已无收益（多出来的线程空转）。

## 3. 锁竞争分析

当前架构的锁盘点：

| 锁 | 作用域 | 是否在热路径 |
|----|--------|-------------|
| （无）shard 存储 | 每 shard 一个 worker，无锁 | 否（这正是 Week 3 的核心收益）|
| `AofWriter` 互斥锁 | 全局，所有写命令共享 | 仅 `appendonly yes` 时 |
| `EventLoop` 互斥锁 | 跨线程 `RunInLoop` | 是，但临界区极短（入队）|
| `Logger` 互斥锁 | 全局 | 仅打日志时 |

- **存储层零锁**：每个 `Shard` 只被它的 `ShardWorker` 线程访问，`SET/GET/INCR/LPUSH` 全程
  无锁，这是 12–13 万 QPS 的根基。对比 Week 1 的单 `Keyspace` + `std::shared_mutex`：写走独占
  锁，所有 IO 线程串行，写并发一高就退化成单线程。
- **AOF 互斥锁是唯一明显的写路径争用点**：8 个 shard 都往一个 `AofWriter` 追加，`Append`
  开头 `lock_guard` 串行进入临界区。`appendfsync always` 时还在**持锁状态下 fsync**（毫秒
  级），锁被占住期间其他 shard 全部排队——这是 Week 4 压测里 `always` 只有 ~180 ops/s 的
  根因；`everysec`/`no` 只在持锁下 `write`（微秒级），吞吐不受影响。结论：**生产用
  `everysec`，`always` 只在「一条都不能丢」的强一致场景**。
- **调优方向**（已实现/可讲）：`LruClock()` 缓存避免热路径时钟 syscall；shard 数取 2–8；
  AOF 用 `everysec`。未做（记录在案）：`AofWriter` 双缓冲/无锁 ring buffer、CPU 亲和、
  io_uring 真正落地（`src/io/uring_io.cpp` 目前是占位）。

## 4. 稳定性与泄漏

- **ASan/UBSan**（`-DMYCACHE_ENABLE_SANITIZERS=ON`）：78 个单测 + `mycache_stress 200000`
  全绿，无内存错误、无泄漏。
- **valgrind**（`--leak-check=full`，`mycache_stress 20000`）：`100,038 allocs == 100,038
  frees`，`0 errors`。跟踪分配器 + 引用计数对象在引擎层没有泄漏路径。
- **长稳**：`scripts/soak_test.sh [seconds]` 循环 SET/GET/DEL/EXPIRE/淘汰并周期打印 RSS 与
  `INFO`，用于 2–4h 级长稳；RSS 应稳定在 `maxmemory` 附近不随时间上涨（涨说明泄漏）。

## 5. 已知边界

- 采样淘汰在「表很大、key 极少」时可能够不到残余 key，无法精确压到最后一个字节（见
  `docs/eviction.md`）。
- `mem_fragmentation_ratio` 在数据集很小时虚高（RSS 含线程栈/libc）。
- 单条 round-trip 延迟未做 `SORT` 类复杂命令的 profile；本报告聚焦 GET/SET/INCR/LPUSH
  热路径。
