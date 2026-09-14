#!/usr/bin/env bash
# 第一阶段的端到端验收检查：PING/SET/GET/DEL + 流水线。
# 要求 PATH 中有 redis-cli。从项目根目录（或任意位置）运行。
set -u
cd "$(dirname "$0")/.."

PORT="${1:-6379}"
LOG="$(mktemp)"
CONF="$(mktemp)"
# 用给定端口生成临时配置（避免与机器上其它 6379 进程冲突）。
sed "s/^port .*/port $PORT/" conf/mycache.conf > "$CONF"

./build/mycache-server "$CONF" > "$LOG" 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null; rm -f "$LOG" "$CONF"' EXIT

# 等待服务端开始响应 PING。
ready=0
for _ in $(seq 1 50); do
  if redis-cli -p "$PORT" ping >/dev/null 2>&1; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "server did not become ready"; cat "$LOG"; exit 1
fi

echo "== PING =="; redis-cli -p "$PORT" ping
echo "== SET foo bar =="; redis-cli -p "$PORT" set foo bar
echo "== GET foo =="; redis-cli -p "$PORT" get foo
echo "== DEL foo =="; redis-cli -p "$PORT" del foo
echo "== GET foo (after DEL, expect nil) =="; redis-cli -p "$PORT" get foo
echo "== command name case-insensitive (set / SET / Get on the same key) =="
redis-cli -p "$PORT" set foo 1
redis-cli -p "$PORT" SET foo 2
redis-cli -p "$PORT" Get foo
echo "== unknown command =="; redis-cli -p "$PORT" nosuchcommand
echo "== arity error =="; redis-cli -p "$PORT" set onlykey

echo "== pipeline (RESP batch via redis-cli --pipe) =="
{
  printf '*1\r\n$4\r\nPING\r\n'
  printf '*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n'
  printf '*2\r\n$3\r\nGET\r\n$1\r\nk\r\n'
  printf '*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n'
} | redis-cli -p "$PORT" --pipe

echo "== Week 3: hash (HSET/HGET/HGETALL/HDEL) =="
redis-cli -p "$PORT" hset u name alice
redis-cli -p "$PORT" hset u age 30
redis-cli -p "$PORT" hget u name
redis-cli -p "$PORT" hgetall u
redis-cli -p "$PORT" hdel u age
redis-cli -p "$PORT" hgetall u

echo "== Week 3: WRONGTYPE =="
redis-cli -p "$PORT" set str val
redis-cli -p "$PORT" hget str f

echo "== Week 3: TTL (EXPIRE/TTL) =="
redis-cli -p "$PORT" set temp v
redis-cli -p "$PORT" expire temp 100
redis-cli -p "$PORT" ttl temp

echo "== Week 3: TTL expiry (key becomes invisible) =="
redis-cli -p "$PORT" set expiring v
redis-cli -p "$PORT" pexpire expiring 100
sleep 0.3
redis-cli -p "$PORT" get expiring

echo "== Week 3: multi-key routing (keys across shards) =="
for i in $(seq 1 64); do redis-cli -p "$PORT" set "r:$i" "v$i" >/dev/null; done
redis-cli -p "$PORT" get "r:1"
redis-cli -p "$PORT" get "r:37"
redis-cli -p "$PORT" get "r:64"

echo "== Week 5: INCR/DECR =="
redis-cli -p "$PORT" incr cnt
redis-cli -p "$PORT" incr cnt
redis-cli -p "$PORT" decr cnt
redis-cli -p "$PORT" get cnt

echo "== Week 5: list (LPUSH/LRANGE/LLEN) =="
redis-cli -p "$PORT" lpush mylist a b c
redis-cli -p "$PORT" lrange mylist 0 -1
redis-cli -p "$PORT" llen mylist

echo "== Week 5: INFO / DBSIZE =="
redis-cli -p "$PORT" dbsize
redis-cli -p "$PORT" info | grep -a -E "used_memory|maxmemory_policy|keyspace_hits|db0:"

echo "== benchmark (20000 req, one pipelined batch) =="
./build/mycache_bench 127.0.0.1 "$PORT" 20000

echo "== server log (tail) =="
tail -n 20 "$LOG"
