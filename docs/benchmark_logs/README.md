# benchmark_logs

`scripts/bench_sweep.sh` 生成的原始压测输出（未做任何过滤/摘要）。`docs/performance.md`
里的表格数字均取自本目录，可直接逐行核对。

## 命名规则

`<bench>_io<io_threads>_shard<shard_count>.log`

| 前缀 | 含义 | 记录内容 |
|------|------|----------|
| `redis-benchmark_` | `redis-benchmark -p <port> -t set,get -n 200000 -c 50 -d 64` | 完整输出（含百分位直方图），`requests per second` 两行依次为 SET、GET |
| `get_set_` | `mycache_get_set_bench -p <port> -n 20000 -d 64` | 单连接 round-trip 的 QPS + P50/P99 |
| `pipeline_` | `mycache_pipeline_bench 127.0.0.1 <port> 200000` | 单连接深度流水线的吞吐上限 |
| `concurrency_` | `mycache_concurrency_bench -p <port> -c 50 -n 4000 -d 64` | 多连接多线程聚合 QPS + P50/P99 |

## 5 档配置（端口）

| io_threads | shard_count | 端口 |
|-----------|-------------|------|
| 1 | 1 | 6381 |
| 4 | 1 | 6382 |
| 4 | 2 | 6384 |
| 4 | 4 | 6385 |
| 4 | 8 | 6383 |

## 复现

```bash
cmake --build build -j"$(nproc)"
./scripts/bench_sweep.sh
```

> 注意：WSL2 上绝对吞吐随 CPU 频率/宿主负载漂移（2–8 倍），同一 sweep 内的相对结论稳定。
> 若重跑覆盖了本目录日志，请同步更新 `docs/performance.md` 里对应的表格数字。
