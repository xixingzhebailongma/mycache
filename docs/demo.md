# 演示脚本（Week 8）

一份**照着念就能流畅演示**的顺序清单。每个环节标注「要指出的点」——演示不是跑命令，是每个
命令背后讲一句「为什么 / 这里有个坑」。

## 0. 前置

```sh
./scripts/build.sh            # 构建（默认链接 jemalloc）
redis-cli -p 6380 ping        # 确认 redis-cli 可用（服务端还没起，会 Connection refused，正常）
```

## 1. 启动 + 核心命令（1 分钟）

```sh
./build/mycache-server conf/mycache.conf   # 前台跑，另开一个终端继续
```

```sh
redis-cli -p 6379 set foo bar      # +OK
redis-cli -p 6379 get foo          # "bar"
redis-cli -p 6379 incr counter     # 1（int 编码，值塞进 8 字节 ptr，不额外 malloc）
redis-cli -p 6379 lpush l a b c    # 3
redis-cli -p 6379 lrange l 0 -1    # c / b / a
redis-cli -p 6379 hset u name alice
redis-cli -p 6379 info             # 指 used_memory / mem_fragmentation_ratio / keyspace_hits
```

**要指出的点**：INCR 走整数编码；INFO 的 `mem_fragmentation_ratio = RSS / used_memory`；
hash/list 都在 `used_memory` 统计内（因为走 zmalloc 而非 std::deque/std::string）。

## 2. 分片路由（30 秒）

```sh
for i in $(seq 1 64); do redis-cli -p 6379 set "r:$i" "v$i" >/dev/null; done
redis-cli -p 6379 get "r:37"     # 命中，无论 key 落在哪个 shard
```

**要指出的点**：`DictHash(key) % shard_count` 路由到固定 worker，同一 key 的命令在单线程 FIFO
执行，所以存储无锁——这是 13 万 QPS 的根基。

## 3. 崩溃恢复 demo（kill -9，~10 秒）

```sh
./scripts/crash_test.sh 6390
#    [always] OK / [everysec] OK / [no] OK
```

**要指出的点**：三种 fsync 策略 `kill -9` 后数据都**不丢**，因为每次都 `write()` 进页缓存；
fsync 管的是**断电**。AOF 每条记录 `[len][crc32][payload]`，靠长度+校验和确定性判断半条记录并
截断修复。

## 4. AOF rewrite demo（~5 秒）

```sh
./scripts/rewrite_test.sh 6392
#   rewrite_test OK: rewrite 期间并发写入不丢、kill -9 后数据完整
```

手动版（更直观）：

```sh
redis-cli -p 6392 bgrewriteaof   # 触发 fork + COW 子进程压缩日志
```

**要指出的点**（三连坑）：① fork 后子进程只剩调用线程，所以子进程**全程无分配**（否则 malloc
锁被消失的线程持有会死锁）；② fork 后的并发写由父进程 **rewrite buffer** 接住，rename 前合并，
一条不丢；③ fork 瞬间用 `SnapshotGate` 保证没有 shard 任务在途（避免读到 rehash 半搬迁的撕裂
dict）。

## 5. RDB demo（~5 秒）

```sh
./scripts/rdb_test.sh 6393
#   rdb_test OK: SAVE/BGSAVE 写快照、kill -9 后重启从 RDB 恢复
```

**要指出的点**：`SAVE` 阻塞写、`BGSAVE` fork 写，都写 temp + 原子 rename（崩溃不留半截）；启动
加载 AOF 优先、否则 RDB。

## 6. 淘汰 demo（~15 秒）

```sh
./scripts/eviction_test.sh 6394   # 填满 maxmemory -> 自动淘汰 -> INFO 看 evicted_keys
```

**要指出的点**：近似 LRU 采样淘汰（采样 N 个 key 淘汰最旧），不是精确 LRU 的全局链表；`LruClock`
是缓存的原子量（100ms 刷新），避免热路径时钟 syscall。

## 7. 压测（~10 秒）

```sh
redis-benchmark -p 6379 -t get,set,incr,lpush -n 200000 -c 50 -d 64
```

**要指出的点**：这是**流水线**口径（每个客户端灌大量命令、批量回包），测服务端吞吐上限；单条
round-trip 受网络 RTT 主导。13 万 QPS 说明热路径无锁、瓶颈在分配+哈希；同机原生 Redis 约 20 万
（本项目 ~60–65%）。

## 8. 稳定性验收（可选，后台）

```sh
./scripts/soak_test.sh 60 6396      # 60s 长稳；RSS 稳定即无泄漏
```

---

## 一张图记住主线

```
client ──TCP──> main thread(accept) ──round-robin──> IO reactor(parse RESP, route)
                                                          │ DictHash(key) % shard_count
                                                          ▼
                                                   ShardWorker(无锁读写 shard)
                                                          │
                                     ┌────────────────────┼────────────────────┐
                                     ▼                    ▼                    ▼
                                 AofWriter             eviction           Connection::Send
                              (append+fsync)        (采样淘汰 LRU)        (跨线程回跳 flush)
```
