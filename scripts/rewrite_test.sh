#!/usr/bin/env bash
# AOF rewrite 验收：rewrite 期间并发写入不丢、kill -9 后重启数据完整。
# 要求 PATH 中有 redis-cli。用法：scripts/rewrite_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6392}"
dir="$(mktemp -d)"
conf="$dir/mycache.conf"
log="$dir/server.log"

cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 4
log_level info
appendonly yes
appendfsync everysec
dir $dir
appendfilename appendonly.aof
EOF

cleanup() {
  [ -n "${srv:-}" ] && kill -9 "$srv" 2>/dev/null
  [ -n "${srv2:-}" ] && kill -9 "$srv2" 2>/dev/null
  rm -rf "$dir"
}
trap cleanup EXIT

wait_ready() {
  for _ in $(seq 1 100); do
    redis-cli -p "$PORT" ping >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "server not ready"; cat "$log"; return 1
}

./build/mycache-server "$conf" > "$log" 2>&1 &
srv=$!
wait_ready || exit 1

# 基线数据
for i in $(seq 0 199); do redis-cli -p "$PORT" set "k$i" "v$i" >/dev/null; done

# 触发 rewrite，同时并发写入 200 个 key（覆盖 rewrite 期间写入不丢的验收点）
redis-cli -p "$PORT" bgrewriteaof >/dev/null
for i in $(seq 200 399); do redis-cli -p "$PORT" set "k$i" "v$i" >/dev/null; done

# 等 rewrite 完成（子进程退出 + 父进程合并 buffer + rename）
for _ in $(seq 1 100); do grep -q "AOF rewrite completed" "$log" && break; sleep 0.1; done

# kill -9 重启
kill -9 "$srv"; wait "$srv" 2>/dev/null
./build/mycache-server "$conf" >> "$log" 2>&1 &
srv2=$!
wait_ready || exit 1

ok=1
db="$(redis-cli -p "$PORT" dbsize)"
[ "$db" = "400" ] || { echo "FAIL: dbsize=$db (want 400)"; ok=0; }
[ "$(redis-cli -p "$PORT" get k0)" = "v0" ] || { echo "FAIL: k0 lost"; ok=0; }
[ "$(redis-cli -p "$PORT" get k250)" = "v250" ] || { echo "FAIL: k250 lost"; ok=0; }
[ "$(redis-cli -p "$PORT" get k399)" = "v399" ] || { echo "FAIL: k399 lost"; ok=0; }

if [ "$ok" = 1 ]; then
  echo "rewrite_test OK: rewrite 期间并发写入不丢、kill -9 后数据完整"
else
  echo "rewrite_test FAILED"
  exit 1
fi
