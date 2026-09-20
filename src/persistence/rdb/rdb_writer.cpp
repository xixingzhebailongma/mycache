#include "mycache/persistence/rdb/rdb_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "mycache/persistence/aof/aof_format.h"  // aof::Crc32
#include "mycache/server/command_router.h"
#include "mycache/storage/collection/hash.h"
#include "mycache/storage/collection/list.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/object/object.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

namespace {

// ---- 输出流（写 fd，满了就刷）----
struct OutBuf {
  int fd;
  char* base;
  size_t cap;
  size_t n;
};

bool WriteFd(int fd, const void* p, size_t len) {
  const char* c = static_cast<const char*>(p);
  size_t off = 0;
  while (off < len) {
    ssize_t w = ::write(fd, c + off, len - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

bool Flush(OutBuf* o) {
  if (o->n == 0) return true;
  if (!WriteFd(o->fd, o->base, o->n)) return false;
  o->n = 0;
  return true;
}

bool OutAppend(OutBuf* o, const void* p, size_t len) {
  if (len > o->cap) return false;
  if (o->n + len > o->cap && !Flush(o)) return false;
  std::memcpy(o->base + o->n, p, len);
  o->n += len;
  return true;
}

// ---- 数字格式化（栈上，无分配）----
size_t U64toa(uint64_t v, char* out) {
  char tmp[24];
  size_t i = 0;
  if (v == 0) {
    out[0] = '0';
    return 1;
  }
  while (v) {
    tmp[i++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }
  for (size_t j = 0; j < i; ++j) out[j] = tmp[i - 1 - j];
  return i;
}

size_t I64toa(int64_t v, char* out) {
  if (v < 0) {
    out[0] = '-';
    return 1 + U64toa(0 - static_cast<uint64_t>(v), out + 1);
  }
  return U64toa(static_cast<uint64_t>(v), out);
}

// ---- 单条记录 payload 暂存 + 打包成 [len][crc][payload] ----
struct RecBuf {
  char* base;
  size_t cap;
  size_t n;
};

bool RecAppend(RecBuf* r, const void* p, size_t len) {
  if (r->n + len > r->cap) return false;
  std::memcpy(r->base + r->n, p, len);
  r->n += len;
  return true;
}

void RecU8(RecBuf* r, uint8_t v) { RecAppend(r, &v, 1); }

void RecU32(RecBuf* r, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                  static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
  RecAppend(r, b, 4);
}

void RecU64(RecBuf* r, uint64_t v) {
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  RecAppend(r, b, 8);
}

void RecBytes(RecBuf* r, const char* data, size_t len) {  // [len][bytes]
  RecU32(r, static_cast<uint32_t>(len));
  RecAppend(r, data, len);
}

void RecStringValue(RecBuf* r, const Object* val) {
  if (val->encoding == ObjEncoding::kInt) {
    char b[24];
    size_t n = I64toa(ObjectPtrToLongLong(val->ptr), b);
    RecBytes(r, b, n);
  } else {
    const char* s = static_cast<const char*>(val->ptr);
    RecBytes(r, s, sdslen(s));
  }
}

bool FinishRecord(RecBuf* r, OutBuf* o) {
  uint32_t len = static_cast<uint32_t>(r->n);
  uint32_t crc = aof::Crc32(r->base, len);
  unsigned char hdr[8];
  hdr[0] = len & 0xFFu;
  hdr[1] = (len >> 8) & 0xFFu;
  hdr[2] = (len >> 16) & 0xFFu;
  hdr[3] = (len >> 24) & 0xFFu;
  hdr[4] = crc & 0xFFu;
  hdr[5] = (crc >> 8) & 0xFFu;
  hdr[6] = (crc >> 16) & 0xFFu;
  hdr[7] = (crc >> 24) & 0xFFu;
  if (!OutAppend(o, hdr, 8)) return false;
  return OutAppend(o, r->base, r->n);
}

// ---- 每条 key 的 emit ----
struct EmitCtx {
  RecBuf* rec;
  OutBuf* out;
  bool failed;
};

void EmitHashValue(EmitCtx* c, const Object* val) {
  Dict* hd = HashDict(const_cast<Object*>(val));
  RecU32(c->rec, static_cast<uint32_t>(hd->Size()));
  for (int t = 0; t < 2; ++t) {
    const HashTable& ht = hd->Table(t);
    if (!ht.table) continue;
    for (size_t i = 0; i < ht.size; ++i) {
      for (DictEntry* he = ht.table[i]; he; he = he->next) {
        const char* f = static_cast<const char*>(he->key);
        const Object* fv = static_cast<const Object*>(he->val);
        RecBytes(c->rec, f, sdslen(f));
        RecStringValue(c->rec, fv);
      }
    }
  }
}

void EmitListValue(EmitCtx* c, const Object* val) {
  List* l = ListPtr(const_cast<Object*>(val));
  RecU32(c->rec, static_cast<uint32_t>(l->len));
  for (ListNode* n = l->head; n; n = n->next) {
    RecBytes(c->rec, n->value, sdslen(n->value));
  }
}

void EmitKey(const char* key, size_t key_len, const Object* val, int64_t ttl_ms, void* ud) {
  EmitCtx* c = static_cast<EmitCtx*>(ud);
  if (c->failed) return;
  c->rec->n = 0;

  bool ok = true;
  switch (val->type) {
    case ObjType::kString:
      RecU8(c->rec, kRdbTypeString);
      RecU8(c->rec, ttl_ms >= 0 ? 1 : 0);
      if (ttl_ms >= 0) RecU64(c->rec, static_cast<uint64_t>(ttl_ms));
      RecBytes(c->rec, key, key_len);
      RecStringValue(c->rec, val);
      break;
    case ObjType::kHash:
      RecU8(c->rec, kRdbTypeHash);
      RecU8(c->rec, ttl_ms >= 0 ? 1 : 0);
      if (ttl_ms >= 0) RecU64(c->rec, static_cast<uint64_t>(ttl_ms));
      RecBytes(c->rec, key, key_len);
      EmitHashValue(c, val);
      break;
    case ObjType::kList:
      RecU8(c->rec, kRdbTypeList);
      RecU8(c->rec, ttl_ms >= 0 ? 1 : 0);
      if (ttl_ms >= 0) RecU64(c->rec, static_cast<uint64_t>(ttl_ms));
      RecBytes(c->rec, key, key_len);
      EmitListValue(c, val);
      break;
    default:
      return;  // set/zset 未实现
  }

  if (ok) ok = FinishRecord(c->rec, c->out);
  if (!ok) c->failed = true;
}

}  // namespace

bool RdbWriteSnapshot(CommandRouter* router, const char* path,
                      char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap) {
  int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;

  OutBuf out{fd, out_buf, out_cap, 0};
  RecBuf rec{rec_buf, rec_cap, 0};
  EmitCtx ctx{&rec, &out, false};

  bool ok = OutAppend(&out, kRdbMagic, kRdbMagicSize);
  if (ok) ok = OutAppend(&out, &kRdbVersion, 1);

  for (size_t i = 0; ok && !ctx.failed && i < router->shard_count(); ++i) {
    router->ShardAt(i)->ScanSnapshot(EmitKey, &ctx);
  }

  if (ok && !ctx.failed) ok = OutAppend(&out, &kRdbEof, 1);
  if (ok && !ctx.failed) ok = Flush(&out);
  if (ok) ::fsync(fd);  // 快照落盘后才算完成

  ::close(fd);
  return ok && !ctx.failed;
}

void WriteRdbChild(CommandRouter* router, const char* path,
                   char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap) {
  bool ok = RdbWriteSnapshot(router, path, out_buf, out_cap, rec_buf, rec_cap);
  _exit(ok ? 0 : 1);
}

}  // namespace mycache
