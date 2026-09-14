# mycache

A from-scratch Redis-compatible in-memory key-value server in C++17, built for
learning. `redis-cli` is the reference client.

## Status (Week 5)

Memory governance + performance stability + engineering, on top of the Week 1–4
engine (sharded storage, dict/rehash, TTL, AOF + crash recovery).

- **Maxmemory + eviction** — a per-shard, sampling-based eviction engine with five
  `maxmemory-policy` values (`noeviction` / `allkeys-lru` / `allkeys-random` /
  `volatile-lru` / `volatile-random`). `used_memory()` is the single source of truth;
  each shard enforces the shared water level by evicting only its own hash-assigned
  keys. LRU is an approximate sampler (sample N keys, evict the least-recently-used)
  backed by a cached LRU clock refreshed every 100 ms.
- **`INFO` + metrics** — `used_memory`/`used_memory_human`, `maxmemory`,
  `mem_fragmentation_ratio` (RSS/used), `keyspace_hits`/`misses`/ratio,
  `evicted_keys`, `expired_keys`, `connected_clients`, per-shard key/expire counts.
- **New commands** — `INCR`/`DECR`/`INCRBY`/`DECRBY`, and a `zmalloc`-backed list type
  with `LPUSH`/`RPUSH`/`LPOP`/`RPOP`/`LRANGE`/`LLEN`, `INFO`, `DBSIZE`.
- **Benchmarked** — `redis-benchmark` GET/SET/INCR/LPUSH run at ~130k req/s with
  sub-millisecond P99 (see `docs/benchmark.md`).
- **Validated** — 78 GoogleTest unit tests clean under ASan/UBSan; valgrind shows zero
  leaks/errors on the engine; crash-recovery demo (Week 4) still green.

## Build (Linux/WSL)

```sh
./scripts/build.sh
```

Requirements: CMake >= 3.16, a C++17 compiler, and network access to fetch
googletest at configure time. For a sanitizer build:

```sh
cmake -S . -B build-asan -DMYCACHE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
```

## Run

```sh
./build/mycache-server conf/mycache.conf
```

Then, in another terminal:

```sh
redis-cli ping                 # PONG
redis-cli set foo bar          # OK
redis-cli get foo              # "bar"
redis-cli incr counter         # (integer) 1
redis-cli lpush l a b c        # (integer) 3
redis-cli lrange l 0 -1        # c / b / a
redis-cli info                 # server/memory/stats/keyspace
redis-cli dbsize               # (integer) N
```

Enable a memory limit and eviction in `conf/mycache.conf`:

```
maxmemory 200mb
maxmemory_policy allkeys-lru
maxmemory_samples 5
```

With `maxmemory_policy noeviction`, writes that would exceed the limit return
`-OOM command not allowed when used memory > 'maxmemory'`.

Enable AOF persistence (`appendonly yes`) for crash recovery (see Week 4).

## Test & benchmark

```sh
./build/mycache_tests           # 78 unit tests (incl. eviction/list/info/LRU)
./build/mycache_stress 1000000  # in-process million-key SET/GET + memory stats
./build/mycache_aof_bench 50000 # AOF fsync policy throughput comparison
./scripts/acceptance.sh [port]  # end-to-end redis-cli acceptance (routing/hash/TTL/info/eviction)
./scripts/eviction_test.sh [port]  # maxmemory eviction demo (fill -> auto-evict -> INFO)
./scripts/crash_test.sh [port]  # kill -9 recovery across all three fsync policies
./scripts/recovery_test.sh [port]  # restart recovery + torn-tail repair
./scripts/benchmark.sh          # pipelined SET/GET throughput over the network
./scripts/soak_test.sh [seconds] [port]  # long-stability SET/GET/DEL/EXPIRE + RSS monitor
```

Benchmark against a running server with `redis-benchmark`:

```sh
redis-benchmark -t get,set,incr,lpush -n 200000 -c 50 -d 64
```

## Layout

```
include/mycache/   public headers (common, net, protocol, command, storage, server, eviction, monitor)
src/               implementation
tests/unit/        GoogleTest unit tests
benchmark/         client-side benchmarks
conf/              sample configuration
docs/              design notes (architecture, persistence, eviction, benchmark, interview)
```
