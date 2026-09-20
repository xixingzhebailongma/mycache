#include "mycache/persistence/aof/aof_rewrite.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "mycache/persistence/aof/aof_format.h"
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

// ---- 单条记录的 payload 暂存 + 打包成 [len][crc][payload] ----
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

bool RecCh(RecBuf* r, char c) { return RecAppend(r, &c, 1); }
bool RecCRLF(RecBuf* r) { return RecAppend(r, "\r\n", 2); }

bool RecInt(RecBuf* r, int64_t v) {
  char b[24];
  size_t n = I64toa(v, b);
  return RecAppend(r, b, n);
}

bool RecBulk(RecBuf* r, const char* data, size_t len) {
  return RecCh(r, '$') && RecInt(r, static_cast<int64_t>(len)) && RecCRLF(r) &&
         RecAppend(r, data, len) && RecCRLF(r);
}

bool RecArrayHeader(RecBuf* r, int64_t n) {
  return RecCh(r, '*') && RecInt(r, n) && RecCRLF(r);
}

// 写 [len][crc] 头（小端，与 aof::EncodeRecord / DecodeHeader 一致）+ payload。
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

// 一个字符串值（int 编码 → 十进制；string 编码 → sds 原始字节）追加成 bulk string。
bool RecStringValue(RecBuf* r, const Object* val) {
  if (val->encoding == ObjEncoding::kInt) {
    char b[24];
    size_t n = I64toa(ObjectPtrToLongLong(val->ptr), b);
    return RecBulk(r, b, n);
  }
  const char* s = static_cast<const char*>(val->ptr);
  return RecBulk(r, s, sdslen(s));
}

// ---- 每条 key 的 emit（ScanSnapshot 回调）----
struct EmitCtx {
  RecBuf* rec;
  OutBuf* out;
  bool failed;
};

void EmitSet(EmitCtx* c, const char* key, size_t key_len, const Object* val) {
  c->rec->n = 0;
  bool ok = RecArrayHeader(c->rec, 3) && RecBulk(c->rec, "SET", 3) &&
            RecBulk(c->rec, key, key_len) && RecStringValue(c->rec, val);
  if (ok) ok = FinishRecord(c->rec, c->out);
  if (!ok) c->failed = true;
}

void EmitHash(EmitCtx* c, const char* key, size_t key_len, const Object* val) {
  Dict* hd = HashDict(const_cast<Object*>(val));
  for (int t = 0; t < 2; ++t) {
    const HashTable& ht = hd->Table(t);
    if (!ht.table) continue;
    for (size_t i = 0; i < ht.size; ++i) {
      for (DictEntry* he = ht.table[i]; he; he = he->next) {
        const char* f = static_cast<const char*>(he->key);
        const Object* fv = static_cast<const Object*>(he->val);
        c->rec->n = 0;
        bool ok = RecArrayHeader(c->rec, 4) && RecBulk(c->rec, "HSET", 4) &&
                  RecBulk(c->rec, key, key_len) && RecBulk(c->rec, f, sdslen(f)) &&
                  RecStringValue(c->rec, fv);
        if (ok) ok = FinishRecord(c->rec, c->out);
        if (!ok) {
          c->failed = true;
          return;
        }
      }
    }
  }
}

void EmitList(EmitCtx* c, const char* key, size_t key_len, const Object* val) {
  List* l = ListPtr(const_cast<Object*>(val));
  const size_t kChunk = 128;  // 每个 RPUSH 记录的元素数（限制单条记录大小）
  ListNode* node = l->head;
  size_t emitted = 0;
  while (emitted < l->len) {
    size_t k = l->len - emitted;
    if (k > kChunk) k = kChunk;
    c->rec->n = 0;
    bool ok = RecArrayHeader(c->rec, static_cast<int64_t>(k + 2)) &&
              RecBulk(c->rec, "RPUSH", 5) && RecBulk(c->rec, key, key_len);
    for (size_t i = 0; i < k && ok; ++i, node = node->next) {
      ok = RecBulk(c->rec, node->value, sdslen(node->value));
    }
    if (ok) ok = FinishRecord(c->rec, c->out);
    if (!ok) {
      c->failed = true;
      return;
    }
    emitted += k;
  }
}

void EmitPexpireat(EmitCtx* c, const char* key, size_t key_len, int64_t ttl) {
  c->rec->n = 0;
  char b[24];
  size_t n = I64toa(ttl, b);
  bool ok = RecArrayHeader(c->rec, 3) && RecBulk(c->rec, "PEXPIREAT", 9) &&
            RecBulk(c->rec, key, key_len) && RecBulk(c->rec, b, n);
  if (ok) ok = FinishRecord(c->rec, c->out);
  if (!ok) c->failed = true;
}

void EmitKey(const char* key, size_t key_len, const Object* val, int64_t ttl_ms, void* ud) {
  EmitCtx* c = static_cast<EmitCtx*>(ud);
  if (c->failed) return;
  switch (val->type) {
    case ObjType::kString:
      EmitSet(c, key, key_len, val);
      break;
    case ObjType::kHash:
      EmitHash(c, key, key_len, val);
      break;
    case ObjType::kList:
      EmitList(c, key, key_len, val);
      break;
    default:
      return;  // set/zset 尚未实现，当前不会出现
  }
  if (!c->failed && ttl_ms >= 0) EmitPexpireat(c, key, key_len, ttl_ms);
}

}  // namespace

void RewriteAofChild(CommandRouter* router, const char* temp_path,
                     char* out_buf, size_t out_cap, char* rec_buf, size_t rec_cap) {
  int fd = ::open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) _exit(1);

  OutBuf out{fd, out_buf, out_cap, 0};
  RecBuf rec{rec_buf, rec_cap, 0};
  EmitCtx ctx{&rec, &out, false};

  // 文件头（与 AofWriter::Open 写的一致）。
  if (!OutAppend(&out, aof::kMagic, aof::kMagicSize)) _exit(1);

  for (size_t i = 0; i < router->shard_count(); ++i) {
    const Shard* shard = router->ShardAt(i);
    shard->ScanSnapshot(EmitKey, &ctx);
    if (ctx.failed) break;
  }

  if (!ctx.failed && !Flush(&out)) ctx.failed = true;
  if (ctx.failed) {
    ::close(fd);
    _exit(1);
  }

  ::fsync(fd);
  ::close(fd);
  _exit(0);
}

}  // namespace mycache
