#!/usr/bin/env bash
# AOF 恢复测试：优雅重启数据不丢 + 尾部不完整记录（partial write）修复。
# 要求 PATH 中有 redis-cli。用法：scripts/recovery_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6391}"
dir="$(mktemp -d)"
conf="$dir/mycache.conf"
log="$dir/server.log"
aof="$dir/appendonly.aof"

cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 4
log_level warn
appendonly yes
appendfsync everysec
dir $dir
appendfilename appendonly.aof
EOF

cleanup() {
  [ -n "${srv:-}" ] && kill "$srv" 2>/dev/null
  [ -n "${srv2:-}" ] && kill "$srv2" 2>/dev/null
  rm -rf "$dir"
}
trap cleanup EXIT

wait_ready() {
  local srv="$1"
  for _ in $(seq 1 100); do
    redis-cli -p "$PORT" ping >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "server not ready"; cat "$log"; return 1
}

# ---- 1) 写入数据，优雅停止（SIGTERM → fsync + close）----
./build/mycache-server "$conf" > "$log" 2>&1 &
srv=$!
wait_ready "$srv" || exit 1

redis-cli -p "$PORT" set one 1 >/dev/null
redis-cli -p "$PORT" set two 2 >/dev/null
redis-cli -p "$PORT" hset h f v >/dev/null

kill "$srv" 2>/dev/null
wait "$srv" 2>/dev/null

[ -s "$aof" ] || { echo "FAIL: AOF file not written"; exit 1; }
size_before=$(wc -c < "$aof")

# ---- 2) 向 AOF 尾部追加一个"只有记录头、没有载荷"的半条记录，模拟 kill -9 写坏 ----
# 小端 len = 4096（\x00\x10\x00\x00）+ crc = 0，但后面没有 4096 字节载荷。
printf '\x00\x10\x00\x00\x00\x00\x00\x00' >> "$aof"

# ---- 3) 重启：应重放完整记录并修复（截断）尾部垃圾 ----
./build/mycache-server "$conf" >> "$log" 2>&1 &
srv2=$!
wait_ready "$srv2" || exit 1

ok=1
[ "$(redis-cli -p "$PORT" get one)" = "1" ] || { echo "FAIL: one lost"; ok=0; }
[ "$(redis-cli -p "$PORT" get two)" = "2" ] || { echo "FAIL: two lost"; ok=0; }
[ "$(redis-cli -p "$PORT" hget h f)" = "v" ] || { echo "FAIL: hash lost"; ok=0; }

# 尾部垃圾应被截断：修复后的文件不大于原有效长度（最多多出文件头 8 字节的重写差异）。
size_after=$(wc -c < "$aof")
if [ "$size_after" -gt "$size_before" ]; then
  echo "FAIL: AOF tail not truncated (before=$size_before after=$size_after)"; ok=0
fi

kill "$srv2" 2>/dev/null
wait "$srv2" 2>/dev/null

if [ "$ok" = 1 ]; then
  echo "recovery_test OK: 优雅重启不丢数据 + partial write 尾部修复"
else
  echo "recovery_test FAILED"
  exit 1
fi
