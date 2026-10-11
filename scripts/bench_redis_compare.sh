#!/usr/bin/env bash
# 同机对比：mycache vs 原生 redis-server，同一 redis-benchmark 参数，产出双方原始日志 + QPS。
#
# 用法：scripts/bench_redis_compare.sh [mycache_port=6389] [redis_port=6390] [req=200000]
#
# 口径：
#   - 两侧都用同一份 redis-benchmark 7.2.5（与原生 server 同版本），同 -t/-n/-c/-d。
#   - mycache 默认 io_threads 4 / shard_count 8（与 docs/performance.md 一致），AOF off。
#   - 原生 redis 关 RDB/AOF（--save '' --appendonly no），纯内存口径，与 mycache 的 AOF off 对齐。
#   - 单次 run，绝对值受 WSL2 漂移影响，只当量级参考；要更稳可连跑 3 次取中位。
set -euo pipefail

MC_PORT=${1:-6389}
RD_PORT=${2:-6390}
N=${3:-200000}
C=50
D=64

REDIS_SRC=/tmp/e2e-infra/redis-src/src
if [[ -x "$REDIS_SRC/redis-server" && -x "$REDIS_SRC/redis-benchmark" ]]; then
  REDIS_SERVER="$REDIS_SRC/redis-server"
  REDIS_BENCH="$REDIS_SRC/redis-benchmark"
else
  REDIS_SERVER=$(command -v redis-server)     || { echo "需要原生 redis-server"; exit 1; }
  REDIS_BENCH=$(command -v redis-benchmark)   || { echo "需要 redis-benchmark"; exit 1; }
fi

LOG=docs/benchmark_logs
mkdir -p "$LOG"

echo "== 环境 =="
"$REDIS_SERVER" --version 2>&1 | head -1
"$REDIS_BENCH" --version 2>&1 | head -1
./build/mycache-server --version 2>/dev/null || echo "(mycache-server 无 --version，跳过)"

bench() { "$REDIS_BENCH" -t set,get -n "$N" -c "$C" -d "$D" -p "$1"; }

wait_port() { for _ in $(seq 1 100); do ss -tln 2>/dev/null | grep -q ":$1 " && return 0; sleep 0.05; done; return 1; }

# 1) mycache-server（默认 io_threads 4 / shard_count 8）
cat >/tmp/mycache_cmp.conf <<CONF
port $MC_PORT
bind 127.0.0.1
io_threads 4
shard_count 8
log_level error
CONF
./build/mycache-server /tmp/mycache_cmp.conf >/tmp/mycache_cmp_srv.log 2>&1 &
MC_PID=$!

# 2) 原生 redis-server 7.2.5（关持久化，纯内存）
"$REDIS_SERVER" --port "$RD_PORT" --bind 127.0.0.1 --save '' --appendonly no --daemonize no >/tmp/redis_cmp_srv.log 2>&1 &
RD_PID=$!

cleanup() { kill "$MC_PID" "$RD_PID" 2>/dev/null || true; }
trap cleanup EXIT

wait_port "$MC_PORT" || { echo "mycache 未监听 $MC_PORT"; cat /tmp/mycache_cmp_srv.log; exit 1; }
wait_port "$RD_PORT" || { echo "redis 未监听 $RD_PORT"; cat /tmp/redis_cmp_srv.log; exit 1; }

# 3) 同参数各跑一次，落盘原始日志
bench "$MC_PORT" > "$LOG/redis_compare_mycache.log" 2>&1
bench "$RD_PORT"  > "$LOG/redis_compare_redis.log"  2>&1

echo
echo "== mycache (port $MC_PORT) =="
grep -E 'requests per second' "$LOG/redis_compare_mycache.log"
echo "== redis   (port $RD_PORT) =="
grep -E 'requests per second' "$LOG/redis_compare_redis.log"
