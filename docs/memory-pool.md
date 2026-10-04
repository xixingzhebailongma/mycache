# 内存池（zmalloc/zfree 层池化）

在 `zmalloc/zfree` 层对小对象做 size-class 池化，省掉 `malloc/free/malloc_usable_size`
的簿记。**不是性能大杀器**——callgrind 基线显示：jemalloc 下分配器簇占 ~26% 指令，
但其中只有 zmalloc/zfree 路径（~5.7%）是池能碰到的，其余 ~20% 走 std::string/
std::vector 的 `operator new/delete`，池碰不到。

## 1. size class 表

| class | block = class+16 | 映射区间 |
|------|-----------------|---------|
| 16 | 32 | 1..16 |
| 32 | 48 | 17..32 |
| 48 | 64 | 33..48 |
| 64 | 80 | 49..64 |
| 96 | 112 | 65..96 |
| 128 | 144 | 97..128 |
| 192 | 208 | 129..192 |
| 256 | 272 | 193..256 |
| 384 | 400 | 257..384 |
| 512 | 528 | 385..512 |
| 768 | 784 | 513..768 |
| 1024 | 1040 | 769..1024 |
| 1536 | 1552 | 1025..1536 |
| 2048 | 2064 | 1537..2048 |
| 3072 | 3088 | 2049..3072 |
| 4096 | 4112 | 3073..4096 |

- 全为 16 的倍数：`block = class + 16` 仍是 16 倍数，glibc 按 16 粒度取整零浪费。
- min class = 16：8 配 16B 头 payload 效率只有 8/24 = 33%，砍掉。

## 2. header 布局与对齐

```
[ PoolHeader 16B ][ user payload class B ]
^ base(malloc 返回，16 对齐)   ^ user = base + 16（仍 16 对齐）
```

```cpp
struct PoolHeader {
  uint64_t magic;       // 8B
  uint32_t size_class;  // 4B
  uint32_t flags;       // 4B 保留
};                      // == 16B
```

**header 必须是 16 的倍数**：`user = base + sizeof(header)`，base 16 对齐，故 header 为
16 或 32 都能保持 user 16 对齐；**24 不行**（`base+24 ≡ 8 mod 16`，破坏 SSE/long double）。

## 3. 4KB 阈值依据

1. 页边界：4096 = 一个 page，glibc 的 tcache/smallbin 上界约 1KB，再往上是大块/按页。
2. 引擎热点（sds 头、dict entry ~32B、list node、object 头）都在几十~几百字节。
3. RSS 成本：池化块 flush 前不还 OS，>4KB 单块滞留即浪费数 KB；cap 在 4KB 封顶。

`size == 0` 与 `size > 4096` 一律直连（`size==0` 走 `malloc(0)`，大块走 `malloc` 并按
`malloc_usable_size` 记账）。

## 4. cap 公式（空闲块预算）

```
cap_blocks(c) = min(1024, 64 KiB / c)          // 单 class 空闲字节 ≤ min(c×1024, 64 KiB)
kBudgetPerThread = 8 MiB                        // 每线程总硬顶（计 block = class+16）
```

- per-class cap 把 16 档全满压到 ≈ 928 KiB/线程；8 MiB 硬顶是公式改版后的保险丝。
- 超预算的块裸 `free()` 归还 OS，不进 freelist。

## 5. magic 校验（重要 caveat）

`zfree/zrealloc` 读 `ptr[-16..-1]` 判断是否池块。对真实 malloc 块这是**技术性 UB**
（glibc 下可读不崩）；magic 命中是**相邻 chunk 用户数据的结构化碰撞**，不是 2⁻⁶⁴ 随机
误判——概率极低但**不为零**。故再叠加 `size_class` 区间校验，失败一律按真实块走 `free()`，
绝不 crash。

## 6. 不变量与 flush 策略

- **同线程 alloc/free**：thread_local 池，块必须由同一线程分配、同一线程释放。跨线程
  free 的池块 magic 校验失败，安全降级为裸 `free()`。
- **POD 无析构**：池是 POD，显式 `PoolFlushAll()` 归还空闲块，不依赖 thread_local 析构
  （静态对象析构期仍可能调 zfree 会 crash）。
- **flush 点**：`main()` 退出前（main 线程）+ 每个 `ShardWorker` 线程 lambda 末尾
  （`loop_.Loop()` 返回之后）。IO reactor 不碰 zmalloc，无需 flush。
- **used_memory 归零**：空闲块在 zfree 时就已下账，`PoolFlushAll` 只归还物理内存，不动
  `used_memory`。

## 7. zrealloc 边界

| 场景 | 行为 |
|-----|------|
| `ptr == nullptr` | `zmalloc(size)` |
| `size == 0` | `zfree(ptr)`，返回 nullptr |
| 池块，`size <= old_class` | 原地返回 ptr |
| 池块 → 更大池块 / 大块 | 新块 + 拷贝 + `zfree(old)` |
| 真实块 → 池块（`size<=4096`） | 新池块 + 拷贝 + 裸 `free(old)` |
| 真实块 → 真实块（`size>4096`） | 原 `std::realloc`（保留 in-place） |

注：池只记 class（上界）不记精确 size，拷贝按上界来（可能多拷几字节，安全）。

## 8. A/B 度量纪律

- 同一二进制、同一时段、同一脚本连跑，`MYCACHE_DISABLE_POOL=1` vs 默认交替，各 2~3 次取中位。
- 主口径：**allocator 路径指令占比下降 X%**（callgrind `zmalloc/zfree` inclusive 差值）。
- 辅口径：QPS 变化（不 headline）。A/B 不设 maxmemory（或设远大于实际用量），避免池化
  记账偏移污染淘汰路径。

## 9. 结论定位

micro-opt + 简历 bullet + 工程练习。真实大鱼在 parser(~32.6%) + std::function(~27%) +
vector<RespValue>，池做完转那里。
