# 性能报告（Performance）

> 合并自 `docs/benchmark.md`（Week 5）与 `docs/benchmark-week7.md`（Week 7），并补充
> Upgrade 1 的三份基准客户端与「并发模型 / shard 数量」实测。本文所有表格数字均来自
> `docs/benchmark_logs/` 里的一次完整 sweep（命令见 §9），**不是只有摘要**——每个数字
> 都能在对应日志里查到原始输出。

## 1. 测试环境

| 项 | 值 |
|----|----|
| CPU | 13th Gen Intel Core i9-13900HX（WSL2 内可见 32 vCPU） |
| 内存 | 7.8 GiB（WSL2 分配） |
| OS | Linux 6.18.40.1-microsoft-standard-WSL2（Ubuntu 22.04 用户态） |
| 编译器 | gcc/g++ 11.4.0，Release `-O3 -DNDEBUG` |
| 分配器 | jemalloc（`libjemalloc.so.2`，`MYCACHE_USE_JEMALLOC=ON`） |
| IO 引擎 | io_uring（`liburing.so.2`，已探测启用） |
| 服务端默认 | `io_threads 4` / `shard_count 8`（见 `conf/mycache.conf`） |
| 压测客户端 | `redis-benchmark`（走本机 TCP）+ 三份自研客户端（见 §2） |

## 2. 测试方法 / 原始命令

三份自研客户端（`benchmark/`，构建产物在 `build/`）：

| 客户端 | 语义 | 用法 |
|--------|------|------|
| `mycache_get_set_bench` | 单连接、逐条 round-trip（in-flight=1），测真实往返延迟 | `./build/mycache_get_set_bench [-h host] [-p port] [-n nreq] [-d size]` |
| `mycache_pipeline_bench` | 单连接、深度流水线（in-flight=N），测吞吐上限 | `./build/mycache_pipeline_bench [host] [port] [nreq]` |
| `mycache_concurrency_bench` | 多连接多线程（每连接 in-flight=1），测聚合 QPS + P50/P99 | `./build/mycache_concurrency_bench [-h host] [-p port] [-c conns] [-t threads] [-n per_conn] [-d size]` |

三者互补：`get_set` 给单条延迟，`pipeline` 给吞吐上限，`concurrency` 给并发聚合吞吐与尾延迟。
`benchmark_pipeline.cpp` 由 `benchmark/pipeline_test.cpp` 迁移而来（行为等价，见文件头注释）。

> 说明：
> - `redis-benchmark` 启动时会打 `ERR unknown command 'config'` / `WARN: could not fetch
>   server CONFIG`——这是它尝试用 `CONFIG GET` 探测服务端，mycache 未实现 `CONFIG` 命令，
>   **不影响后续压测**。
> - `benchmark/latency_test.cpp` 与 `benchmark/memory_test.cpp` 目前是**空占位文件**，暂未
>   纳入本次 Upgrade 1，后续补延迟分布与内存占用专项压测。

## 3. 吞吐与延迟（redis-benchmark，50 并发）

```
redis-benchmark -p <port> -t set,get -n 200000 -c 50 -d 64
```

默认档（`io_threads 4` / `shard_count 8`，端口 6383）：

| 命令 | QPS | P99 |
|------|-----|-----|
| SET | 102,249 | 亚毫秒（99% 分位 ≤ 0.7ms） |
| GET | 95,831 | 亚毫秒 |

结论性的点是「**亚毫秒 P99 + 10 万级 QPS**」。参照：同机原生 `redis-server 7.2.5` 同参数
约 20 万 QPS，本项目纯学习实现做到其 ~50%，差距主要在网络/解析层与 §6.1 的发送锁争用。

## 4. shard 数量调优对比

固定 `io_threads 4`，扫 `shard_count = 1/2/4/8`（端口 6382/6384/6385/6383）。

### 4.1 redis-benchmark（50 并发，SET/GET QPS）

| shard_count | SET QPS | GET QPS |
|-------------|---------|---------|
| 1 | 109,769 | 105,932 |
| 2 | 106,667 | 111,235 |
| 4 | 112,486 | 110,375 |
| 8 | 102,249 | 95,831 |

**redis-benchmark 下吞吐对 shard 数不敏感**（1–8 都在 ~10–11 万）。

### 4.2 mycache_pipeline_bench（单连接、深度流水线 200k，吞吐上限）

| shard_count | SET req/s | GET req/s |
|-------------|-----------|-----------|
| 1 | 160,526 | 231,556 |
| 2 | 84,072 | 99,704 |
| 4 | 64,188 | 60,312 |
| 8 | 44,869 | 46,721 |

**这里出现一个反直觉但可稳定复现的信号**：单连接深度流水线下，shard 越多吞吐**单调下降**
（1→8 约 3–5 倍）。根因见 §6.1——同一连接的回包被多个 shard worker 并发 `conn->Send()`，
命中每连接一把的发送锁；shard=1 时只有 1 个 worker 顺序回包、零争用。深度流水线把所有
命令一次性灌入，把这种争用放大到极致。

## 5. 版本 / 并发模型演进对比

> ⚠️ **重要口径（务必看清，勿误解为历史版本实测）**：
> git 历史里只保留了当前分片版代码（`main` 上 4 个 commit）。Week 1「单线程 Reactor +
> 全局 Keyspace + `shared_mutex`」、Week 2「多 Reactor」的**历史代码版本不在仓库里**，
> 无法原样复跑。**下表数据是当前代码库通过配置降级（`io_threads` / `shard_count`）近似
> 得到，历史版本未复跑。**它反映的是「当前分片实现」在不同并发配置下的表现，不是三个
> 不同代码版本的演进轨迹。

数据来自 `mycache_concurrency_bench -c 50 -n 4000 -d 64`（每阶段 20 万请求），原始输出见
`docs/benchmark_logs/concurrency_*.log`：

| 配置（io_threads/shard_count） | 近似「并发模型」 | SET QPS | GET QPS | P99(SET) | P99(GET) | shard 数 | 备注 |
|-------------------------------|------------------|---------|---------|----------|----------|----------|------|
| 1/1 | 单 Reactor + 单 shard | 35,707 | 38,622 | 1.88ms | 1.74ms | 1 | 1 个 reactor 解析 50 连接全部命令，成为瓶颈 |
| 4/1 | 多 Reactor + 单 shard | 102,380 | 105,364 | 1.05ms | 1.03ms | 1 | 4 个 reactor 分摊解析；单 worker 顺序回包零争用 |
| 4/8 | 多 Reactor + Shard（lock-free）| 59,920 | 61,477 | 1.61ms | 1.59ms | 8 | 8 个 worker 并发回包，撞每连接发送锁（§6.1） |

**解读（重要，面试别背错）**：当前代码的瓶颈**不在存储层**——存储热路径确实是无锁分片。
真正的争用在**网络回包路径**：多 shard 时一个连接的回复由多个 worker 线程并发写出，共享
每连接一把发送锁，反而比单 shard 慢。因此「Shard + lock-free」这一档在本基准里**没有体现
吞吐收益**，它的收益在别处（见 §6.1）。**为什么 shard=8 在这个测试下反而慢**的标准答法：
shard 消除的是「全局 Keyspace 锁 + 多 key 命令并行扇出 + 淘汰分摊」，而本负载（固定 key、
无多 key、无淘汰）不触发这些收益点，只白白多付了「回包跨 worker 抢发送锁」的代价。

## 6. 瓶颈分析

### 6.1 每连接发送锁（本基准暴露出的最大争用点，不是存储层）

- 数据流：reactor 解析命令 → `router.Route(key)` 按 `DictHash(key) % shard_count` 投递到
  某 shard worker → worker 线程执行 handler → handler 里 `conn->Send()` 回包。
- 当 `shard_count > 1`，**同一个连接**的多个 key 会分散到不同 worker，导致多个 worker
  线程并发调用同一连接的 `Send()`，共享该连接的发送锁（`Connection` 内一把 mutex）。
- 后果：单连接深度流水线时 8 个 worker 抢 1 把锁，吞吐从 ~160k（shard1）掉到 ~45k
  （shard8）；50 连接 round-trip 时同样被拖累（4/8 ~60k < 4/1 ~102k）。
- **shard 的收益不是「提吞吐」**，而是：① 每个 shard 无锁、串行处理自己的 key，避免
  Week 1 单全局 `Keyspace + shared_mutex` 在高并发写时的退化；② 为多 key 命令
  （`MGET/MSET/DEL`）提供按 shard 分组的并行扇出；③ 淘汰/过期按 shard 分摊。本基准的
  负载（固定 key、无多 key、无淘汰）恰好**不触发**这些收益点。
- 后续方向（记录在案，未改）：`Connection::Send` 改成「每 reactor 单写者 + 跨 worker 用
  无锁队列/eventfd 汇聚回包」，或按 reactor 而非 shard 决定回包线程，即可消除此争用。

### 6.2 存储热路径（callgrind 采样，Week 7）

热路径 = **分配 + 哈希 + sds 字符串**，全程无锁。最大单项是追踪分配器里的
`malloc_usable_size`（每次 alloc 与 free 都调一次）。可优化（未改）：仿 Redis `zmalloc`
把 usable size 存进分配块头部（`PREFIX_SIZE`），预计砍掉热路径 ~7–10% 指令。

> 该方向已在 **Upgrade 2 以 size-class 池化实现**（见 §10）：实测 allocator 路径指令占比
> 下降 0.78 pp（6.56% → 5.78%），**未达当初 ~7–10% 的乐观预估**——jemalloc 本身已有
> tcache，且池碰不到 std 容器的 `operator new/delete`（占分配簇大头）。

### 6.3 AOF 写路径

- `AofWriter` 互斥锁是唯一明显的写路径争用点：8 个 shard 往一个 writer 追加，`Append`
  开头 `lock_guard` 串行。
- `appendfsync always`：持锁 fsync（毫秒级），吞吐 ≈ 1/fsync 延迟（~190 ops/s）；
  `everysec`/`no`：只在锁内 `write()`（微秒级），吞吐回到 250–300 万 ops/s。
- 结论：**生产用 `everysec`，`always` 只在「一条都不能丢」的强一致场景**。

## 7. 稳定性与泄漏

| 工具 | 构建 | 结果 |
|------|------|------|
| ASan + UBSan | `-DMYCACHE_ENABLE_SANITIZERS=ON` | 92 单测全绿；`mycache_stress 200000` 无泄漏 |
| valgrind | 默认构建 | `100,038 allocs == 100,038 frees`，`0 errors` |
| ThreadSanitizer | `-DMYCACHE_ENABLE_TSAN=ON` | 并发负载（4 reactor + 8 worker + 淘汰 + fork）0 data race |

TSan 在 Linux 6.x 高熵 ASLR 下需 `setarch $(uname -m) -R` 关 ASLR 再跑（本环境免提权）。

## 8. 已知边界

- 采样淘汰在「表很大、key 极少」时可能够不到残余 key（见 `docs/eviction.md`）。
- `mem_fragmentation_ratio` 在数据集很小时虚高（RSS 含线程栈/libc）。
- jemalloc 短期 RSS 略高（粗 size class），长稳下靠 `dirty_decay` 平衡。
- 单条 round-trip 延迟、`SORT` 类复杂命令仍未 profile；perf 需 `sudo apt install
  linux-tools-generic`，本环境以 callgrind 等效替代。

## 9. 原始日志与复现

**原始日志**：`docs/benchmark_logs/`（20 个文件，5 档配置 × 4 种压测，完整原始输出，
命名 `<bench>_io<io>_shard<shard>.log`；Upgrade 2 的 A/B 见 `upgrade2_qps_ab.log` 与
`upgrade2_callgrind_ab.log`）。本文所有表格数字可直接在对应日志里核对。

**一键复现**：`scripts/bench_sweep.sh`（对 5 档配置依次跑 4 种压测并落盘日志）：

```bash
cmake --build build -j"$(nproc)"   # 或从零：rm -rf build && cmake -B build && cmake --build build -j"$(nproc)"
./scripts/bench_sweep.sh
```

> **关于数字波动**：WSL2 上绝对吞吐随 CPU 频率/宿主负载在数倍范围内漂移（同一 sweep 内部
> 相对结论稳定，跨时段跑出的绝对值可能差 2–8 倍）。因此**跨版本对比只认同一批日志里的
> 相对关系**，单次绝对值仅供量级参考。复现时以 `docs/benchmark_logs/` 当次输出为准。

## 10. Upgrade 2：zmalloc/zfree 层 size-class 池化

在 `zmalloc/zfree` 层对小对象（≤4096B）做 size-class 池化，省掉 `malloc/free/
malloc_usable_size` 簿记。设计见 `docs/memory-pool.md`。A/B 用同一二进制运行时切换：
`MYCACHE_DISABLE_POOL=1` 关池（=改动前口径），默认开池。

### 10.1 callgrind 指令占比 A/B（主口径）

同二进制、同 conf（`io_threads 1` / `shard_count 1`，AOF off）、同负载
`redis-benchmark -t set -n 20000 -c 50 -d 64`：

| 指标 | pool OFF | pool ON | Δ |
|------|----------|---------|----|
| 总指令 Ir | 234,988,938 | 233,864,335 | −0.48% |
| `zmalloc` inclusive | 3.60% | 3.01% | −0.59 pp |
| `zfree` inclusive | 2.96% | 2.77% | −0.19 pp |
| **allocator 路径合计** | **6.56%** | **5.78%** | **−0.78 pp（相对自身 −12%）** |

原始日志：`docs/benchmark_logs/upgrade2_callgrind_ab.log`。

### 10.2 QPS A/B（辅口径，软信号）

同二进制、`io_threads 4` / `shard_count 8`、`redis-benchmark -t set,get -n 200000 -c 50 -d 64`，
各 3 次取中位：

| 命令 | pool OFF | pool ON | Δ |
|------|----------|---------|----|
| SET | 101,420 | 105,932 | +4.4%（单次方差 ~10%，软信号） |
| GET | 103,413 | 103,573 | ~0（噪声） |

原始日志：`docs/benchmark_logs/upgrade2_qps_ab.log`。

### 10.3 诚实结论

- 池 ROI 上限 ~5.7%（`zmalloc+zfree` inclusive），实得 **0.78 pp** 总指令占比下降。
- QPS SET +4.4% 但方差 ~10%，**不写进简历**，仅 docs 留档。
- `malloc_usable_size`（~2.23%）是池能消的最大单项，但池自身开销（magic 校验 +
  freelist push/pop）吃掉一部分，净得 0.78 pp。

简历 bullet（锁定，不改）：
> 在 zmalloc/zfree 层做 size-class 池化，allocator 路径指令占比从 6.56% 降到 5.78%
> （相对降低 ~12%），总指令数减少 ~0.5%。
