# mycache

A from-scratch Redis-compatible in-memory key-value server in C++17, built for
learning. `redis-cli` is the reference client.

## Status (Week 7)

Week 7 closes the loop on performance + stability: jemalloc integration, a
ThreadSanitizer build, and the full acceptance run — single-machine QPS 10w+,
sub-millisecond P99, zero leaks, zero data races (see `docs/benchmark-week7.md`).
Built on Week 1–6 (sharded storage, dict/rehash, TTL, AOF + crash recovery +
rewrite, eviction, metrics, list, RDB, pluggable IO engine).

- **AOF rewrite** — `BGREWRITEAOF` forks a COW child that compacts the log; concurrent
  writes land in a rewrite buffer and merge before an atomic `rename`, so no data is
  lost across a rewrite.
- **RDB** — `SAVE` (blocking) / `BGSAVE` (background fork) write a CRC-checked binary
  snapshot; startup loads it when AOF is off.
- **IOEngine** — `io_engine posix|io_uring` routes AOF write/fsync through a pluggable
  engine (`PosixIO`, or liburing-backed `UringIO` when available).

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
- **Validated** — 92 GoogleTest unit tests clean under ASan/UBSan; valgrind shows zero
  leaks/errors on the engine; crash-recovery demo (Week 4) still green.
- **Week 7** — `jemalloc` (default ON, lowers fragmentation + per-thread tcache) and a
  `MYCACHE_ENABLE_TSAN=ON` build for data-race detection; TSan clean under a concurrent
  load (reactors + shard workers + eviction + `BGSAVE` fork). Full report:
  `docs/benchmark-week7.md`.

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

For a ThreadSanitizer (data-race) build, add `-DMYCACHE_ENABLE_TSAN=ON`; on a
Linux 6.x kernel run the binary under `setarch $(uname -m) -R` to work around the
high-entropy ASLR / TSan `unexpected memory mapping` issue. To disable jemalloc
(A/B allocator comparison), pass `-DMYCACHE_USE_JEMALLOC=OFF`.

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
redis-cli bgrewriteaof         # compact the AOF (Background append only file rewriting started)
redis-cli save                 # blocking RDB snapshot
redis-cli bgsave               # background RDB snapshot
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

Enable RDB snapshots (`dbfilename dump.rdb`) for `SAVE`/`BGSAVE`, and pick an IO
engine (`io_engine posix` or `io_uring` — the latter needs `liburing-dev` installed).

## Test & benchmark

```sh
./build/mycache_tests           # 92 unit tests (incl. AOF rewrite / RDB / io_engine)
./build/mycache_stress 1000000  # in-process million-key SET/GET + memory stats
./build/mycache_aof_bench 50000 # AOF fsync policy throughput comparison
./build/mycache_io_bench 20000  # IOEngine write+fsync (posix vs io_uring)
./scripts/acceptance.sh [port]  # end-to-end redis-cli acceptance (routing/hash/TTL/info/eviction)
./scripts/eviction_test.sh [port]  # maxmemory eviction demo (fill -> auto-evict -> INFO)
./scripts/crash_test.sh [port]  # kill -9 recovery across all three fsync policies
./scripts/recovery_test.sh [port]  # restart recovery + torn-tail repair
./scripts/rewrite_test.sh [port]  # AOF rewrite: concurrent writes during rewrite, kill -9 recovery
./scripts/rdb_test.sh [port]  # RDB SAVE/BGSAVE + restart-load from dump.rdb
./scripts/benchmark.sh          # pipelined SET/GET throughput over the network
./scripts/soak_test.sh [seconds] [port]  # long-stability SET/GET/DEL/EXPIRE + RSS monitor
```

Benchmark against a running server with `redis-benchmark`:

```sh
redis-benchmark -t get,set,incr,lpush -n 200000 -c 50 -d 64
```

For a scripted, talk-track-ready walkthrough (crash recovery → AOF rewrite → RDB →
eviction → benchmark), see `docs/demo.md`.

## Deploy

The server is a single static binary + one config file — no runtime deps beyond the
C/C++ stdlib (optionally `liburing` / `libjemalloc`). To run as a service:

```sh
# systemd unit (excerpt)
[Service]
ExecStart=/opt/mycache/build/mycache-server /etc/mycache/mycache.conf
Restart=on-failure
LimitNOFILE=65536

# or, ad hoc daemonization
nohup ./build/mycache-server conf/mycache.conf >> /var/log/mycache.log 2>&1 &
```

Production config pointers (`conf/mycache.conf`):
- `appendonly yes` + `appendfsync everysec` — durability (crash-safe) vs throughput;
  `always` is ~190 ops/s (fsync per record), `everysec` is ~3M ops/s.
- `dbfilename dump.rdb` — periodic `BGSAVE` cold backup; startup loads AOF first, else RDB.
- `maxmemory 200mb` + `maxmemory_policy allkeys-lru` — cap RSS; evict rather than OOM.
- `io_engine posix` (default) / `io_uring` (needs `liburing-dev`).
- `dir /var/lib/mycache` — put AOF/RDB files on the data volume, not the code dir
  (a stray `dump.rdb` in the CWD is auto-loaded on startup).

## Layout

```
include/mycache/   public headers (common, net, protocol, command, storage, server, eviction, monitor)
src/               implementation
tests/unit/        GoogleTest unit tests
benchmark/         client-side benchmarks
conf/              sample configuration
docs/              design notes (architecture, persistence, eviction, benchmark-week7, demo,
                   interview + interview-answer-key)
```
