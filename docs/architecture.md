# Architecture (Week 1)

## Threading model: main reactor + IO reactors

```
                  ┌───────────────┐
 client ──TCP────►│  main thread  │  epoll (ET) on listen fd, accept4()
                  │  main_loop_   │
                  └──────┬────────┘
                         │ round-robin hand-off (RunInLoop)
       ┌─────────────────┼─────────────────┐
       ▼                 ▼                 ▼
 ┌───────────┐     ┌───────────┐     ┌───────────┐
 │ IO reactor│     │ IO reactor│     │ IO reactor│   each: own thread + epoll (ET)
 │ conns...  │     │ conns...  │     │ conns...  │
 └───────────┘     └───────────┘     └───────────┘
```

- `EventLoop` — one epoll instance + an `eventfd` wakeup channel + a pending
  functor queue. `RunInLoop`/`QueueInLoop` provide thread-safe hand-off.
- `Channel` — fd + interest events + callbacks; a weak `tie` keeps the owner
  alive during event dispatch (avoids use-after-free on close).
- `Connection` — input/output `Buffer`, ET read-until-EAGAIN and
  write-until-EAGAIN, graceful shutdown after flushing.
- `Acceptor` — non-blocking listen fd; accepts until `EAGAIN`.
- `IOReactor` — runs an `EventLoop` in a worker thread and owns its connections.

## Data flow for one command

1. `Connection::HandleRead` drains the socket, then fires `MessageCallback`.
2. `Server::OnMessage` parses one or more RESP commands from the input buffer.
3. `Server::DispatchCommand` lowercases the name, looks it up in `CommandTable`,
   checks arity, and invokes the handler with a `CommandContext`.
4. The handler reads/writes the shared `Keyspace` and replies via
   `conn->Send(...)`, which appends to the output buffer and enables `EPOLLOUT`.

## Storage

- `sds` — length-prefixed, binary-safe string; `len`/`alloc` in a header
  (`sdshdr8/16/32/64`) right before the buffer, O(1) length and amortized append.
- `Object` — refcounted Redis-style object. String values use one of two
  encodings: `int` (long long inline in `ptr`) or `string` (separately allocated sds).
- `Dict` — separate-chaining hash table holding two tables (`ht[0]`, `ht[1]`) for
  progressive rehash. Grow/shrink allocate `ht[1]` and set `rehashidx`; each write
  then migrates one bucket until `ht[0]` drains and the tables swap.
- `Keyspace` — `std::shared_mutex`-guarded wrapper shared by all IO reactors.
  Readers take a shared lock and are pure reads (no rehash); writers take an
  exclusive lock and advance rehash one bucket per operation.
- `allocator` / `MemoryStats` — a tracking allocator feeding `used_memory()`,
  plus per-encoding object counts.

## Key design decisions (Week 1 scope)

- One global `Keyspace` (locked) rather than per-shard storage; sharding comes
  in a later week.
- Replies are buffered and flushed in bulk, which is what makes pipelining cheap.
- Signal handling uses an `eventfd` + a channel so `Stop()` runs on the loop
  thread; `SIGPIPE` is ignored.
- Rehash is advanced only on the write path (shared-lock reads never mutate the
  dict), which keeps the `shared_mutex` model sound under concurrency.

## Week 3: sharded execution + hash + TTL

The single global `Keyspace` is replaced, at the command layer, by a set of
shards. Each `Shard` owns an independent `dict` (sds -> Object*) and an
`expires` dict (sds -> absolute expiry ms), and is only ever touched by its
own `ShardWorker` thread, so no lock is needed.

```
                    ┌───────────────┐
  client ──TCP─────►│  main thread  │ accept4 + hand-off (round-robin)
                    └──────┬────────┘
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                  ▼
  ┌───────────┐      ┌───────────┐      ┌───────────┐
  │ IO reactor│      │ IO reactor│      │ IO reactor│  parse RESP, route
  └─────┬─────┘      └─────┬─────┘      └─────┬─────┘
        │   key-hash: DictHash(key) % N        │
        ▼                  ▼                  ▼
  ┌───────────┐      ┌───────────┐      ┌───────────┐
  │ ShardWorker│ ... │ ShardWorker│      │ ShardWorker│  own thread + EventLoop
  └─────┬─────┘      └─────┬─────┘      └─────┬─────┘
        └──── reply via Connection::Send (hops back to IO loop)
```

- `CommandRouter` maps `key -> shard` with `DictHash(key) % shard_count` and
  submits the command as a functor to the owning `ShardWorker`'s `EventLoop`.
  Commands for the same key therefore execute in FIFO order on one thread.
- The reply write-back path (`Worker -> IO Reactor`) is `Connection::Send`,
  which detects the cross-thread call and re-queues onto the connection's IO
  loop via `shared_from_this()` to keep the connection alive until flushed.
- Keyless commands (`PING`, `ECHO`) run inline on the IO thread.
- Hash values are `Object`s of type `kHash` whose `ptr` is a nested dict of
  field (sds) -> value (Object*).
- TTL uses lazy deletion (checked on every lookup) plus a simplified active
  expire: each shard's `timerfd` fires every 100 ms and samples a few random
  buckets of the `expires` dict, removing expired keys.
- Known cuts this week: cross-shard `MGET`/`MSET` and `MULTI`; multi-key
  `DEL`/`EXISTS` are routed by the first key and only affect that shard.

## Week 4: AOF persistence + crash recovery

The sharded write path is extended with an append-only command log so the dataset
survives a `kill -9`. See `docs/persistence.md` for the full design; the essentials:

```
 command ──► ShardWorker ──► mutate Shard ──► AofWriter::Append(record) ──► appendonly.aof
                                                          ▲
                              write() every append        │
                              fsync: always/everysec/no   │
                                                          │
   startup ──► AofRecovery::Replay ──► validate + replay + repair partial tail
```

- `AofWriter` — one shared, mutex-guarded writer. Every record is `write()`n on
  each append (so `kill -9` loses nothing); the fsync policy (`always`/`everysec`/
  `no`) only controls how often the page cache is flushed. `everysec` runs a
  background fsync thread.
- `AofRecovery` — runs at startup before any shard worker thread starts, so it can
  write directly into each `Shard` via `CommandRouter::ShardAt`. Each record is a
  `[len][crc32][RESP command]` frame; a torn tail (partial write from `kill -9`) is
  detected and truncated.
- Write handlers (`SET`/`DEL`/`HSET`/`HDEL`/`EXPIRE`) log through
  `CommandContext::aof`. TTL commands are translated to absolute `PEXPIREAT`, so
  `Shard` expiry now uses wall-clock time (`UnixMs()`).

## Week 5: memory governance + metrics + list

### Eviction (per-shard, sampling-based)

The Week 3 sharded design pays off here: each `Shard` is owned by exactly one
`ShardWorker` thread, so eviction is a plain per-thread operation with **no storage
locks on the hot path**. The flow for a memory-growing write (`SET`/`HSET`/`INCR`/
`LPUSH`/`RPUSH`):

```
command ──► ShardWorker ──► EvictIfNeeded() ──► (over limit?)
                                  │ no: proceed to mutate
                                  │ yes + noeviction: reply OOM
                                  │ yes + lru/random: EvictOne() loop until under
```

- **One source of truth** — `used_memory()` from the tracking allocator. Per-shard
  quota (`maxmemory/shard_count`) vs. per-shard usage (`used_memory()/shard_count`)
  reduces to the *same* global comparison because keys are hash-distributed evenly, so
  each shard simply checks the global water level against its own keys. The tradeoff
  (a hot shard can over-evict its own keys while others are cold) is documented in
  `docs/eviction.md`.
- **Approximate LRU** — `Object` carries a `uint64_t lru` (Redis's robj model), set at
  creation and refreshed on every lookup. A cached `LruClock()` (atomic, refreshed by
  the 100 ms shard timer, mirroring Redis `serverCron`) keeps the hot path free of a
  clock syscall per access. Eviction samples `maxmemory_samples` keys by random-bucket
  walk over both hash tables and deletes the one with the smallest `lru`.
- **Policies** — `noeviction` (default), `allkeys-lru`, `allkeys-random`,
  `volatile-lru`, `volatile-random`. Volatile variants sample the `expires` dict so
  only TTL-bearing keys are evicted.

### Metrics + INFO

A process-wide `Metrics` singleton (atomics) tracks hits/misses, evicted/expired keys,
commands, connections, and uptime. Each `Shard` also keeps atomic `key_count_`/
`expire_count_` (maintained in `Set/Del/DeleteKey/ExpireAt/HSet/HDel`) so `INFO` can
sum cross-shard key/expire counts from the IO thread without a data race. `INFO`
(`monitor/info.cpp`) assembles server/clients/memory/stats/keyspace sections;
`mem_fragmentation_ratio` is `RSS / used_memory` (RSS from `/proc/self/statm`).

### List type + INCR

`INCR`/`DECR`/`INCRBY`/`DECRBY` reuse the integer string encoding and AOF-log the
absolute result as `SET` for deterministic replay. The list type is a plain
doubly-linked list (`ListNode`/`List` in `storage/collection/list.cpp`) where every
node and value goes through `zmalloc` so list memory is correctly counted by
`used_memory()` — unlike a `std::deque`, which would bypass the tracking allocator.


