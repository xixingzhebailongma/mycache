#!/usr/bin/env bash
# 长稳测试：持续 SET/GET/DEL/EXPIRE（带 maxmemory 淘汰），周期打印 RSS/内存水位，
# 用于验证 2~4h 级无泄漏、无崩溃。用法：scripts/soak_test.sh [seconds] [port]
set -u
cd "$(dirname "$0")/.."

DURATION="${1:-300}"
PORT="${2:-6397}"
dir="$(mktemp -d)"
conf="$dir/mycache.conf"
log="$dir/server.log"

cat > "$conf" <<EOF
port $PORT
bind 127.0.0.1
io_threads 4
shard_count 8
log_level warn
maxmemory 64mb
maxmemory_policy allkeys-lru
maxmemory_samples 5
EOF

./build/mycache-server "$conf" > "$log" 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; rm -rf "$dir"' EXIT

for _ in $(seq 1 50); do
  redis-cli -p "$PORT" ping >/dev/null 2>&1 && break
  sleep 0.1
done

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$SRV/status" 2>/dev/null || echo 0; }

echo "== 长稳测试 $DURATION s（每 5s 打印 RSS/key 数）=="
printf "%-8s %-10s %-10s %-10s\n" "t(s)" "RSS(MB)" "keys" "evicted"
start=$(date +%s)
first_rss=0
while true; do
  now=$(date +%s)
  elapsed=$((now - start))
  [ "$elapsed" -ge "$DURATION" ] && break

  # 混合负载：写新 key、读、删、设 TTL。
  for i in $(seq 1 200); do
    redis-cli -p "$PORT" set "s:$((RANDOM % 200000))" "value-$i" >/dev/null 2>&1
    redis-cli -p "$PORT" get "s:$((RANDOM % 200000))" >/dev/null 2>&1
    redis-cli -p "$PORT" del "s:$((RANDOM % 200000))" >/dev/null 2>&1
    redis-cli -p "$PORT" set "e:$((RANDOM % 20000))" "t" >/dev/null 2>&1
    redis-cli -p "$PORT" expire "e:$((RANDOM % 20000))" 5 >/dev/null 2>&1
  done

  rss=$(( $(rss_kb) / 1024 ))
  keys=$(redis-cli -p "$PORT" dbsize 2>/dev/null)
  evicted=$(redis-cli -p "$PORT" info 2>/dev/null | grep -a evicted_keys | cut -d: -f2 | tr -d '\r')
  [ "$first_rss" -eq 0 ] && first_rss=$rss
  printf "%-8s %-10s %-10s %-10s\n" "$elapsed" "$rss" "${keys:-?}" "${evicted:-?}"
  sleep 5
done

# 存活检查 + 简单增长检查（RSS 不应比首个样本翻倍；翻倍暗示泄漏）。
if ! kill -0 "$SRV" 2>/dev/null; then
  echo "soak_test FAILED: server crashed"; tail -20 "$log"; exit 1
fi
last_rss=$(( $(rss_kb) / 1024 ))
echo "RSS first=${first_rss}MB last=${last_rss}MB"
if [ "$last_rss" -gt $(( first_rss * 2 )) ]; then
  echo "soak_test FAILED: RSS grew ${first_rss}MB -> ${last_rss}MB (suspected leak)"
  exit 1
fi
echo "soak_test OK: server survived ${DURATION}s, RSS stable (${first_rss}MB -> ${last_rss}MB)"
