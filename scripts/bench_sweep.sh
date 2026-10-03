#!/usr/bin/env bash
# Upgrade 1 一键复现脚本：对 5 档「io_threads / shard_count」配置，依次跑
# redis-benchmark + 三个自研客户端，原始输出完整落盘到 docs/benchmark_logs/。
#
# 用法：./scripts/bench_sweep.sh
#
# 输出目录结构（每档 4 个日志）：
#   docs/benchmark_logs/
#     redis-benchmark_io1_shard1.log   # redis-benchmark 完整输出（含百分位直方图）
#     get_set_io1_shard1.log           # 单连接 round-trip（QPS + P50/P99）
#     pipeline_io1_shard1.log          # 单连接深度流水线（吞吐上限）
#     concurrency_io1_shard1.log       # 多连接多线程聚合（QPS + P50/P99）
set -euo pipefail
cd "$(dirname "$0")/.."

# io_threads shard_count port
CONFIGS=(
  "1 1 6381"
  "4 1 6382"
  "4 2 6384"
  "4 4 6385"
  "4 8 6383"
)

LOGDIR="docs/benchmark_logs"
mkdir -p "$LOGDIR"

wait_port() {
  local port=$1
  for _ in $(seq 1 100); do
    ss -tln 2>/dev/null | grep -q ":$port " && return 0
    sleep 0.05
  done
  return 1
}

run_config() {
  local io=$1 shard=$2 port=$3
  local conf="/tmp/mycache_io${io}_shard${shard}.conf"
  cat > "$conf" <<CONF
port $port
bind 0.0.0.0
io_threads $io
shard_count $shard
tcp_backlog 511
max_connections 10000
log_level error
CONF

  echo "=== [io_threads=$io shard_count=$shard port=$port] ==="
  ./build/mycache-server "$conf" >"/tmp/mycache_srv_${port}.log" 2>&1 &
  local srv=$!
  if ! wait_port "$port"; then
    echo "server failed to listen on $port"; cat "/tmp/mycache_srv_${port}.log"; kill "$srv" 2>/dev/null || true; exit 1
  fi

  redis-benchmark -p "$port" -t set,get -n 200000 -c 50 -d 64 \
    >"$LOGDIR/redis-benchmark_io${io}_shard${shard}.log" 2>&1
  ./build/mycache_get_set_bench     -p "$port" -n 20000 -d 64 \
    >"$LOGDIR/get_set_io${io}_shard${shard}.log" 2>&1
  ./build/mycache_pipeline_bench     127.0.0.1 "$port" 200000 \
    >"$LOGDIR/pipeline_io${io}_shard${shard}.log" 2>&1
  ./build/mycache_concurrency_bench -p "$port" -c 50 -n 4000 -d 64 \
    >"$LOGDIR/concurrency_io${io}_shard${shard}.log" 2>&1

  kill -TERM "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  sleep 0.2
}

for cfg in "${CONFIGS[@]}"; do
  # shellcheck disable=SC2086
  run_config $cfg
done

echo "done — logs in $LOGDIR/"
ls -la "$LOGDIR"
