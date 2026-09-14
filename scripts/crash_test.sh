#!/usr/bin/env bash
# kill -9 崩溃恢复验收：对三种 appendfsync 策略分别
#   写入数据 -> kill -9 -> 重启 -> 校验数据不丢、删除的 key 不复活、TTL 保留。
# 要求 PATH 中有 redis-cli。用法：scripts/crash_test.sh [port]
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6390}"
POLICIES="always everysec no"

run_one() {
  local policy="$1"
  local dir
  dir="$(mktemp -d)"
  local conf="$dir/mycache.conf"
  local log="$dir/server.log"

  cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 2
shard_count 4
log_level warn
appendonly yes
appendfsync $policy
dir $dir
appendfilename appendonly.aof
EOF

  ./build/mycache-server "$conf" > "$log" 2>&1 &
  local srv=$!

  local ready=0
  for _ in $(seq 1 100); do
    redis-cli -p "$PORT" ping >/dev/null 2>&1 && { ready=1; break; }
    sleep 0.1
  done
  if [ "$ready" != 1 ]; then
    echo "  [$policy] FAIL: server not ready"; cat "$log"; kill "$srv" 2>/dev/null; rm -rf "$dir"; return 1
  fi

  # 写入一批数据：字符串、hash、TTL、以及一个随后被删除的 key。
  redis-cli -p "$PORT" set alpha one >/dev/null
  redis-cli -p "$PORT" set beta two >/dev/null
  redis-cli -p "$PORT" hset hsh fld val >/dev/null
  redis-cli -p "$PORT" set expireme tmp >/dev/null
  redis-cli -p "$PORT" expire expireme 1000 >/dev/null
  redis-cli -p "$PORT" set doomed bye >/dev/null
  redis-cli -p "$PORT" del doomed >/dev/null

  # 等待 everysec 后台 fsync 兜底（write 本身已进入页缓存，kill -9 也不丢）。
  sleep 1.2
  kill -9 "$srv"
  wait "$srv" 2>/dev/null

  # 重启并校验。
  ./build/mycache-server "$conf" >> "$log" 2>&1 &
  local srv2=$!
  for _ in $(seq 1 100); do
    redis-cli -p "$PORT" ping >/dev/null 2>&1 && break
    sleep 0.1
  done

  local ok=1
  [ "$(redis-cli -p "$PORT" get alpha)" = "one" ] || { echo "  [$policy] FAIL: alpha lost"; ok=0; }
  [ "$(redis-cli -p "$PORT" get beta)" = "two" ] || { echo "  [$policy] FAIL: beta lost"; ok=0; }
  [ "$(redis-cli -p "$PORT" hget hsh fld)" = "val" ] || { echo "  [$policy] FAIL: hash lost"; ok=0; }
  [ "$(redis-cli -p "$PORT" exists doomed)" = "0" ] || { echo "  [$policy] FAIL: deleted key resurrected"; ok=0; }
  local ttl; ttl="$(redis-cli -p "$PORT" ttl expireme)"
  [ "$ttl" -gt 0 ] 2>/dev/null || { echo "  [$policy] FAIL: TTL lost (ttl=$ttl)"; ok=0; }

  kill "$srv2" 2>/dev/null
  wait "$srv2" 2>/dev/null
  rm -rf "$dir"

  if [ "$ok" = 1 ]; then
    echo "  [$policy] OK"
  else
    return 1
  fi
}

echo "== kill -9 crash/recovery test (port $PORT) =="
rc=0
for p in $POLICIES; do
  run_one "$p" || rc=1
done

if [ "$rc" = 0 ]; then
  echo "crash_test OK: kill -9 后数据不丢（三种策略均通过）"
else
  echo "crash_test FAILED"
  exit 1
fi
