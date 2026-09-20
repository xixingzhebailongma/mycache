#!/usr/bin/env bash
# RDB 快照验收：SAVE/BGSAVE 写快照、kill -9 后重启从 dump.rdb 恢复。
# 要求 PATH 中有 redis-cli。用法：scripts/rdb_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6393}"
dir="$(mktemp -d)"
conf="$dir/mycache.conf"
log="$dir/server.log"

cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 4
log_level info
appendonly no
dbfilename dump.rdb
dir $dir
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

redis-cli -p "$PORT" set foo bar >/dev/null
redis-cli -p "$PORT" hset h f v >/dev/null
redis-cli -p "$PORT" rpush l a b c >/dev/null
redis-cli -p "$PORT" expire foo 1000 >/dev/null

# SAVE：同步快照
r="$(redis-cli -p "$PORT" save)"
[ "$r" = "OK" ] || { echo "FAIL: SAVE returned '$r'"; exit 1; }
[ -s "$dir/dump.rdb" ] || { echo "FAIL: dump.rdb not written"; exit 1; }

# 再写一点，BGSAVE：异步快照
redis-cli -p "$PORT" set extra more >/dev/null
redis-cli -p "$PORT" bgsave >/dev/null
for _ in $(seq 1 100); do grep -q "RDB background save completed" "$log" && break; sleep 0.1; done

# kill -9 重启，从 dump.rdb 恢复
kill -9 "$srv"; wait "$srv" 2>/dev/null
./build/mycache-server "$conf" >> "$log" 2>&1 &
srv2=$!
wait_ready || exit 1

ok=1
[ "$(redis-cli -p "$PORT" get foo)" = "bar" ] || { echo "FAIL: foo lost"; ok=0; }
[ "$(redis-cli -p "$PORT" hget h f)" = "v" ] || { echo "FAIL: hash lost"; ok=0; }
[ "$(redis-cli -p "$PORT" get extra)" = "more" ] || { echo "FAIL: extra lost"; ok=0; }
[ "$(redis-cli -p "$PORT" llen l)" = "3" ] || { echo "FAIL: list length wrong"; ok=0; }
[ "$(redis-cli -p "$PORT" ttl foo)" -gt 0 ] 2>/dev/null || { echo "FAIL: TTL lost"; ok=0; }

if [ "$ok" = 1 ]; then
  echo "rdb_test OK: SAVE/BGSAVE 写快照、kill -9 后重启从 RDB 恢复"
else
  echo "rdb_test FAILED"
  exit 1
fi
