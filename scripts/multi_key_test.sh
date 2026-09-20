#!/usr/bin/env bash
# 多 key 命令（MSET/MGET/DEL/EXISTS）跨 shard 路由验收：
#   MSET/MGET 顺序、EXISTS 重复 key 语义、DEL 计数、AOF 恢复跨 shard 不丢。
# 要求 PATH 中有 redis-cli。用法：scripts/multi_key_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6395}"
dir="$(mktemp -d)"
conf="$dir/mycache.conf"
log="$dir/server.log"

cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 8
log_level warn
appendonly yes
appendfsync always
dir $dir
appendfilename appendonly.aof
EOF

cleanup() {
  [ -n "${srv:-}" ] && kill -9 "$srv" 2>/dev/null
  [ -n "${srv2:-}" ] && kill "$srv2" 2>/dev/null
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

fail() { echo "FAIL: $1"; exit 1; }

./build/mycache-server "$conf" > "$log" 2>&1 &
srv=$!
wait_ready || exit 1

# 1) MSET 20 key + MGET 顺序。
[ "$(redis-cli -p "$PORT" mset k1 v1 k2 v2 k3 v3 k4 v4 k5 v5 k6 v6 k7 v7 k8 v8 k9 v9 k10 v10 k11 v11 k12 v12 k13 v13 k14 v14 k15 v15 k16 v16 k17 v17 k18 v18 k19 v19 k20 v20)" = "OK" ] || fail "mset"
mgot="$(redis-cli -p "$PORT" mget k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12 k13 k14 k15 k16 k17 k18 k19 k20)"
expected="$(seq 1 20 | sed 's/^/v/')"
[ "$mgot" = "$expected" ] || fail "mget order: got [$mgot]"

# 2) EXISTS：20 个全在，重复 key 每个出现都计数。
[ "$(redis-cli -p "$PORT" exists k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12 k13 k14 k15 k16 k17 k18 k19 k20)" = "20" ] || fail "exists all"
[ "$(redis-cli -p "$PORT" exists k1 k1 k1)" = "3" ] || fail "exists duplicate"
[ "$(redis-cli -p "$PORT" exists nosuch k1)" = "1" ] || fail "exists mixed"

# 3) 跨 shard DEL（k2/k5/k13 哈希分布在不同 shard），计数正确。
[ "$(redis-cli -p "$PORT" del k2 k5 k13)" = "3" ] || fail "del count"
[ "$(redis-cli -p "$PORT" exists k2 k5 k13)" = "0" ] || fail "del actually removed"

# 4) kill -9 后从 AOF 恢复，跨 shard 的删除不复活。
sleep 0.3
kill -9 "$srv"; wait "$srv" 2>/dev/null
./build/mycache-server "$conf" >> "$log" 2>&1 &
srv2=$!
wait_ready || exit 1
[ "$(redis-cli -p "$PORT" dbsize)" = "17" ] || fail "recovery dbsize (want 17)"
[ "$(redis-cli -p "$PORT" exists k2 k5 k13)" = "0" ] || fail "recovery resurrected deleted keys"
[ "$(redis-cli -p "$PORT" mget k1 k3 k4 k6)" = "$(printf 'v1\nv3\nv4\nv6')" ] || fail "recovery values wrong"

echo "multi_key_test OK: MSET/MGET/DEL/EXISTS 跨 shard 正确，AOF 恢复不丢"
