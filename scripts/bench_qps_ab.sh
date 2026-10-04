#!/usr/bin/env bash
# QPS A/B: baseline(21a36f7, pool-off) vs current(main), io4/shard8, 交错跑各 3 次取中位。
# 前置：build-baseline 与 build 均已按 RelWithDebInfo 构建。
# 用法: bash scripts/bench_qps_ab.sh
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=6388
CONF=/tmp/mycache_qps_ab.conf
LOGDIR=docs/benchmark_logs
mkdir -p "$LOGDIR"

cat > "$CONF" <<CONF
port $PORT
bind 0.0.0.0
io_threads 4
shard_count 8
tcp_backlog 511
max_connections 10000
appendonly no
log_level error
CONF

wait_port() {
  for _ in $(seq 1 100); do
    ss -tln 2>/dev/null | grep -q ":$PORT " && return 0
    sleep 0.05
  done
  return 1
}

run_single() {  # $1=label  $2=server  $3=run
  local label=$1 server=$2 run=$3
  local out="$LOGDIR/qps_ab_${label}.log"
  "$server" "$CONF" >"/tmp/mycache_qps_ab_srv.log" 2>&1 &
  local srv=$!
  if ! wait_port "$PORT"; then
    echo "server fail ($label run$run)"; cat /tmp/mycache_qps_ab_srv.log
    kill "$srv" 2>/dev/null || true; exit 1
  fi
  {
    echo "=== $label run$run get_set ==="
    ./build/mycache_get_set_bench -p "$PORT" -n 20000 -d 64
    echo "=== $label run$run concurrency ==="
    ./build/mycache_concurrency_bench -p "$PORT" -c 50 -t 50 -n 4000 -d 64
  } >> "$out"
  kill -TERM "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  sleep 0.2
}

for run in 1 2 3; do
  run_single baseline ./build-baseline/mycache-server "$run"
  run_single current  ./build/mycache-server "$run"
done

echo "done — logs in $LOGDIR/qps_ab_{baseline,current}.log"
