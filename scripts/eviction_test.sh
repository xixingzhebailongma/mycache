#!/usr/bin/env bash
# maxmemory 淘汰演示：写远超上限的 key -> 自动淘汰 -> INFO 展示 evicted_keys 与内存水位，
# 再对比 noeviction 下超限写入回 OOM。要求 PATH 中有 redis-cli。用法：scripts/eviction_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6399}"
dir="$(mktemp -d)"
log="$dir/server.log"

make_conf() {
  local policy="$1"
  cat > "$dir/mycache.conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 4
log_level warn
maxmemory 1mb
maxmemory_policy $policy
maxmemory_samples 5
EOF
}

start_server() {
  ./build/mycache-server "$dir/mycache.conf" > "$log" 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do
    redis-cli -p "$PORT" ping >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "server not ready"; cat "$log"; exit 1
}

stop_server() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

VAL=$(printf 'x%.0s' $(seq 1 128))

echo "== [1/2] allkeys-lru：写 20000 个 ~128B key（远超 1mb），观察自动淘汰 =="
make_conf allkeys-lru
start_server
for i in $(seq 1 20000); do redis-cli -p "$PORT" set "k$i" "$VAL" >/dev/null; done
redis-cli -p "$PORT" info | grep -a -E "used_memory_human|maxmemory:|maxmemory_policy:|evicted_keys|db0:"
stop_server

echo
echo "== [2/2] noeviction：内存超限后写入回 OOM =="
make_conf noeviction
start_server
for i in $(seq 1 20000); do redis-cli -p "$PORT" set "k$i" "$VAL" >/dev/null; done
echo "set 超限 key 的返回：$(redis-cli -p "$PORT" set overflow "$VAL")"
stop_server

rm -rf "$dir"
echo "eviction_test OK"
