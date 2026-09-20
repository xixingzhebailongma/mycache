# 性能优化 + 稳定性（Week 7）

**目标**：单机 QPS 达标（10w+）、P99 < 1ms、无内存泄漏、无数据竞争。
**环境**：WSL2 单机，Release（`-O3`），`redis-benchmark` 走本机 TCP，`io_threads 4` / `shard_count 8`。

## 验收结果（TL;DR）

| 验收项 | 目标 | 实测 |
|--------|------|------|
| 单机 QPS | 10w+ | SET ~121k / GET ~132k / INCR ~124k / LPUSH ~115k ✅ |
| P99 延迟 | < 1ms | SET ≈ 0.7ms / GET < 1ms ✅（99.99% 尾延迟 2–18ms 为 WSL 调度抖动）|
| 内存泄漏 | 无 | ASan/UBSan + valgrind 全绿，`100,038 allocs == 100,038 frees` ✅ |
| 数据竞争 | 无 | TSan 并发负载（4 reactor + 8 worker + 淘汰 + fork）0 warning ✅ |

Week 7 新增代码改动：`jemalloc` 集成（`CMakeLists.txt` 的 `MYCACHE_USE_JEMALLOC` 选项，默认
ON）+ `ThreadSanitizer` 构建选项（`MYCACHE_ENABLE_TSAN`）。

---

## D1 锁竞争分析 + shard 数量调优

存储热路径**零锁**是 Week 3 分片设计的核心收益，本次无需改动，只做复测确认：

| shard_count | GET QPS |
|-------------|---------|
| 1   | ~109k |
| 4   | ~116k |
| 8   | ~118k |
| 16  | ~126k |

结论与 Week 5 一致：**吞吐对 shard 数不敏感**（1–16 都在 ~11–13 万），瓶颈在 IO/网络层而非
shard worker（每个 shard 无锁、串行处理自己的 key）。默认 `8` 是「够散又不至于开太多线程」的
稳妥值。锁盘点与调优方向（AOF 用 `everysec`、`LruClock` 缓存等）见 `docs/benchmark.md` §3。

## D2 jemalloc + 碎片率优化

`zmalloc`（`src/storage/memory/allocator.cpp`）只包装 `std::malloc/free` + `malloc_usable_size`，
因此链接 jemalloc 后这些符号被透明拦截，**无需改动引擎代码**。

- 构建：`MYCACHE_USE_JEMALLOC=ON`（默认）探测 `libjemalloc.so`（dev 包）或 `libjemalloc.so.2`
  （仅运行时包，Ubuntu 装在 `/usr/lib/<multiarch>` 下，`find_library` 默认不搜，故用
  `find_file` + `-print-multiarch` HINTS）。
- 运行时确认：`grep jemalloc /proc/<pid>/maps` 命中 `libjemalloc.so.2`。

### 吞吐（A/B：glibc vs jemalloc，同脚本顺序测）

| 命令 | glibc QPS | jemalloc QPS | 变化 |
|------|-----------|--------------|------|
| SET | ~128k | ~130k | +2% |
| GET | ~124k | ~128k | +3% |

增量来自 jemalloc 的**每线程 tcache**：8 个 shard worker + 4 个 IO reactor 并发分配时，
malloc/free 不再争抢 glibc 的 arena 锁。

### 碎片率（`mem_fragmentation_ratio = RSS / used_memory`）

| 场景 | glibc | jemalloc |
|------|-------|----------|
| 400k key 一次性 fill | 1.26 | 1.16 |
| 100k key × 9 次覆盖（churn）| 1.16 | 1.13 |

**诚实说明**：jemalloc 的 `malloc_usable_size` 按更粗的 size class 上取整，因此它报告的
`used_memory` 更大（400k key 时 67MB vs glibc 55MB），碎片率分子分母都变了。上面「比率下降」
部分来自「可用的定义变宽」。绝对 RSS 在短期微基准里 jemalloc 反而略高（这是它「用内存换
低碎片 + 高并发」的取舍，与 Redis 默认用 jemalloc 的现状一致）。jemalloc 真正的长线收益——
**持续 churn 下 RSS 不漂移**——要在 D6 长稳里体现。

## D3 瓶颈定位（火焰图 / perf）

`perf` 未安装且本环境无 sudo，改用 `valgrind --tool=callgrind` 对存储热路径做调用图采样
（`mycache_stress 20000`），热点函数（按指令数）：

| 占比 | 函数 |
|------|------|
| 7.5% | `malloc_usable_size`（jemalloc）|
| 4.1% | `free` |
| 3.8% | `malloc` |
| 3.7% | `DictHash` |
| ~8%  | `sdslen` / `sdsnewlen` / `sdsHdrSize` |
| ~5%  | `Dict::AddRaw` / `Dict::Rehash` |
| ~4.3%| `zmalloc` / `zfree` / `UsableSize` 包装 |

结论：热路径 = **分配 + 哈希 + sds 字符串操作**，全程无锁。最大的单项成本是追踪分配器里的
`malloc_usable_size`（每次 alloc **和** free 都调一次）。可优化方向（记录在案，未改）：仿 Redis
`zmalloc` 把 usable size 存进分配块头部（`PREFIX_SIZE`），`zfree` 直接读头、省掉
`malloc_usable_size` 调用——预计砍掉热路径 ~7–10% 指令。之所以 Week 7 不改：它动的是地基
分配器，需要单独一轮 ASan/TSan/valgrind 全套回归，风险/收益不成比例（验收已达标）。

## D4 redis-benchmark：GET/SET/INCR/LPUSH

```
redis-benchmark -t get,set,incr,lpush -n 300000 -c 50 -d 64
```

| 命令 | QPS | P99 |
|------|-----|-----|
| SET   | ~121k | ≈ 0.7ms |
| GET   | ~132k | < 1ms |
| INCR  | ~124k | ≈ 2ms（尾）|
| LPUSH | ~115k | ≈ 9ms（尾）|

注：上表 P99 取 99.99% 分位，含 WSL2 调度抖动；99% 分位（Redis 通常报的口径）全部 < 1ms。
绝对数字随 WSL 抖动浮动 ~10%，结论性的点是「亚毫秒 + 12–13 万级 QPS」稳定成立。

## D5 ASan / valgrind / TSan

| 工具 | 构建 | 结果 |
|------|------|------|
| ASan + UBSan | `-DMYCACHE_ENABLE_SANITIZERS=ON` | 92 单测全绿；`mycache_stress 200000` 无泄漏 |
| valgrind | 默认构建 | `100,038 allocs == 100,038 frees`，`0 bytes in use`，`0 errors` |
| ThreadSanitizer | `-DMYCACHE_ENABLE_TSAN=ON` | 并发负载 0 data race warning |

TSan 的实测方式（比单测更有说服力）：把**真实服务端**跑在 TSan 下，用 `redis-benchmark` 打
`get,set,incr,lpush`（50 并发）+ 填到 `maxmemory 32mb` 触发**淘汰**（`evicted_keys=219275`）+
`BGSAVE`（fork + SnapshotGate）——日志 0 行 warning，服务存活。

> TSan 在 Linux 6.x 高熵 ASLR 下会报 `unexpected memory mapping`，需 `setarch $(uname -m) -R`
> 关掉 ASLR 再跑（本环境无 sudo，`setarch -R` 恰好免提权）。

## D6 长稳测试

`scripts/soak_test.sh [seconds]`：混合 SET/GET/DEL/EXPIRE + `maxmemory 64mb` 淘汰，每 5s 打印
RSS / key 数 / evicted，结束断言 RSS 未翻倍（翻倍即疑似泄漏）。

- 60s 验证：RSS `47MB → 48MB` 稳定，`soak_test OK`。
- 15min 背景跑：RSS 全程 47–48MB，无增长。
- 24h：同一脚本 `./scripts/soak_test.sh 86400`；RSS 稳定在 `maxmemory` 附近不随时间上涨即通过。

## io_uring 与 fsync 对比（D2 补充）

`mycache_io_bench 20000`（单条 64B 记录 write+fsync，对应 `appendfsync always` 负载）：

| 引擎 | ops/s |
|------|-------|
| posix    | 186 |
| io_uring | 191 |

结论：**同步 submit+wait 封装下单条 write+fsync 基本持平**（~2.7% 差距，噪声级）。这符合预期
——io_uring 的真正收益在**批提交多个 SQE**（攒一批 write/fsync 一次 submit），当前 `UringIO`
只是打通路径的同步封装，批提交未实现，是记录在案的「已完成 vs 未完成」边界。

`mycache_aof_bench 30000`（AOF 三档 fsync 吞吐）：

| 策略 | ops/s |
|------|-------|
| always   | 193 |
| everysec | 3,010,650 |
| no       | 2,577,700 |

`always` 每次持锁 fsync（毫秒级），吞吐 ≈ 1/fsync 延迟；`everysec`/`no` 只在锁内 `write()`，
吞吐回到 250–300 万。三者都每次 `write()` 进页缓存，所以 `kill -9` 都不丢已 ack 的数据。

## 已知边界 / 后续

- `malloc_usable_size` 追踪是存储热路径最大单项开销 → 后续改 Redis 式头部存 size。
- jemalloc 短期 RSS 略高（粗 size class），长稳下靠 `dirty_decay`/`background_thread` 平衡，可
  后续加 `MALLOC_CONF=background_thread:true` 微调。
- 单条 round-trip 延迟、`SORT` 类复杂命令仍未 profile（本报告聚焦 GET/SET/INCR/LPUSH）。
- perf / 火焰图需 `sudo apt install linux-tools-generic` 才能装，本环境以 callgrind 等效替代。
