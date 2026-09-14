# Persistence: AOF (Week 4)

Append-only file (AOF) persistence records every write command so the dataset can
be reconstructed after a crash. This document describes the file format, the three
fsync policies, crash recovery, and how AOF integrates with the sharded write path.

## Overview

```
 command ──► ShardWorker ──► mutate Shard ──► AofWriter::Append(record)
                        └── reply to client

                          ┌─────────────────────────────┐
                          │  AofWriter (shared, mutex)  │
                          │  write() every append       │
                          │  fsync: always/everysec/no  │
                          └──────────────┬──────────────┘
                                         ▼
                              appendonly.aof (on disk)
```

On startup (before any shard worker thread starts) the server replays the AOF back
into the shards, validates checksums, and repairs a partially-written tail.

## File format

```
 0        8        16                              N
 +--------+--------+------ ... -------+----------------
 | magic  | len(4) | crc(4) | payload | ... more records
 +--------+--------+------ ... -------+----------------
```

- **Magic header** — 8 bytes `MYAOF001`, identifies the file and version.
- **Record** — one command per record:
  - `len` (uint32, little-endian): payload byte count.
  - `crc` (uint32, little-endian): CRC32 (IEEE 802.3) of the payload.
  - `payload`: the command serialized as a RESP array of bulk strings, e.g.
    `*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n`.

Each record is written with a single `WriteAll` loop under the writer's mutex, so
records never interleave across threads. A crash mid-record therefore leaves a
truncated record at the tail, never a corrupted one in the middle.

## fsync policies

All three policies `write()` each record to the OS page cache on every append, so
data acknowledged to a client survives a process `kill -9` regardless of policy.
The policies differ only in when the page cache is flushed to stable storage
(i.e. durability against power loss / OS crash):

| `appendfsync` | fsync frequency | durability on power loss | throughput |
|---------------|-----------------|--------------------------|------------|
| `always`      | every record     | strongest (no loss)       | lowest     |
| `everysec`    | once per second  | lose ≤ 1s                | high       |
| `no`          | never (OS decides)| lose up to OS writeback  | highest    |

`always` fsyncs synchronously inside `Append` (before the reply is sent); `everysec`
uses a dedicated background thread that fsyncs once per second; `no` never fsyncs
(except on graceful shutdown).

## Crash recovery (`AofRecovery::Replay`)

1. Open the file read/write; a missing file is treated as an empty AOF.
2. Validate the magic header (a truncated header resets the file to empty).
3. Read each record: parse `len`/`crc`, read the payload, verify CRC32, parse the
   payload as a RESP command, and apply it to the owning shard.
4. **Partial-write repair** — the last record may be incomplete because `kill -9`
   interrupted its write. Two cases are detected and repaired by truncating the
   file to the last complete record:
   - fewer than 8 bytes remain (partial header), or
   - the header claims more payload than remains (`len` overruns EOF).
   A CRC mismatch on the *final* record is also treated as a torn tail. A CRC
   mismatch on a *middle* record is a fatal error (refuses to start).

Recovery runs single-threaded before shard workers start, so it writes directly
into each `Shard` via `CommandRouter::ShardAt`.

## Commands and TTL

Only state-changing commands are logged: `SET`, `DEL`, `HSET`, `HDEL`, and TTL
changes. Reads are never logged. `DEL`/`HDEL` are logged only when they actually
changed something.

TTL commands are translated to absolute time before logging, so replay is
independent of when it happens:

- `EXPIRE key s` / `PEXPIRE key ms` → `PEXPIREAT key <absolute-ms>`

TTL expiry timestamps therefore use wall-clock time (`UnixMs()`, i.e.
`CLOCK_REALTIME`), not the monotonic clock, so absolute timestamps remain
meaningful across a restart.

## Concurrency

A single `AofWriter` is shared by all shard workers. `Append` takes an internal
mutex, so appends are serialized and never interleave. This is the same
single-log model Redis uses, extended to a multi-threaded shard design. For
`always`, the fsync happens under the same lock, which is the expected cost of
that policy.

## Configuration

```
appendonly yes              # enable AOF (default: no)
appendfilename appendonly.aof
appendfsync everysec        # always | everysec | no
dir .                       # directory for the AOF file
```

## Limitations (future work)

- **No AOF rewrite yet** — the log grows without bound; a background rewrite
  (compacting the log to a point-in-time snapshot) is planned but not part of
  Week 4.
- **Recovery reads the whole file into memory** — fine for the current scale,
  not for very large logs.
- TTL uses wall-clock time; a large NTP jump can shift expiry (same as Redis).
