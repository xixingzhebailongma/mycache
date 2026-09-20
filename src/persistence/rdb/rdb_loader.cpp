#include "mycache/persistence/rdb/rdb_loader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "mycache/persistence/aof/aof_format.h"  // aof::Crc32
#include "mycache/persistence/rdb/rdb_writer.h"  // 格式常量
#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"

namespace mycache {

namespace {

// 从已读入内存的 RDB 缓冲区中顺序解析的游标。
struct Cursor {
  const unsigned char* p;
  size_t n;
  size_t pos = 0;
};

bool Need(Cursor* c, size_t k) { return c->pos + k <= c->n; }

bool ReadU8(Cursor* c, uint8_t* out) {
  if (!Need(c, 1)) return false;
  *out = c->p[c->pos++];
  return true;
}

bool ReadU32(Cursor* c, uint32_t* out) {
  if (!Need(c, 4)) return false;
  const unsigned char* b = c->p + c->pos;
  *out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  c->pos += 4;
  return true;
}

bool ReadU64(Cursor* c, uint64_t* out) {
  if (!Need(c, 8)) return false;
  const unsigned char* b = c->p + c->pos;
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
  *out = v;
  c->pos += 8;
  return true;
}

bool ReadBytes(Cursor* c, std::string* out) {
  uint32_t len = 0;
  if (!ReadU32(c, &len)) return false;
  if (!Need(c, len)) return false;
  out->assign(reinterpret_cast<const char*>(c->p + c->pos), len);
  c->pos += len;
  return true;
}

bool ApplyRecord(CommandRouter* router, const unsigned char* payload, size_t len) {
  Cursor c{payload, len, 0};

  uint8_t type = 0, has_ttl = 0;
  if (!ReadU8(&c, &type) || !ReadU8(&c, &has_ttl)) return false;

  int64_t ttl = -1;
  if (has_ttl) {
    uint64_t t = 0;
    if (!ReadU64(&c, &t)) return false;
    ttl = static_cast<int64_t>(t);
  }

  std::string key;
  if (!ReadBytes(&c, &key)) return false;

  Shard* shard = router->ShardAt(router->ShardIndex(key));
  switch (type) {
    case kRdbTypeString: {
      std::string v;
      if (!ReadBytes(&c, &v)) return false;
      shard->Set(key, v);
      break;
    }
    case kRdbTypeHash: {
      uint32_t n = 0;
      if (!ReadU32(&c, &n)) return false;
      for (uint32_t i = 0; i < n; ++i) {
        std::string f, v;
        if (!ReadBytes(&c, &f) || !ReadBytes(&c, &v)) return false;
        shard->HSet(key, f, v);
      }
      break;
    }
    case kRdbTypeList: {
      uint32_t n = 0;
      if (!ReadU32(&c, &n)) return false;
      std::vector<std::string> elems;
      elems.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::string e;
        if (!ReadBytes(&c, &e)) return false;
        elems.push_back(std::move(e));
      }
      shard->RPush(key, elems);
      break;
    }
    default:
      return false;
  }

  if (c.pos != len) return false;  // payload 有残留字节
  if (ttl >= 0) shard->ExpireAt(key, ttl);
  return true;
}

}  // namespace

RdbLoadResult RdbLoad(const std::string& path, CommandRouter* router) {
  RdbLoadResult r;

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    r.error = "open failed: " + std::string(std::strerror(errno));
    return r;
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    r.error = "fstat failed";
    ::close(fd);
    return r;
  }
  const size_t size = static_cast<size_t>(st.st_size);

  std::vector<unsigned char> buf(size);
  size_t got = 0;
  while (got < size) {
    ssize_t n = ::read(fd, buf.data() + got, size - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      r.error = "read failed";
      ::close(fd);
      return r;
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
  ::close(fd);
  if (got != size) {
    r.error = "short read";
    return r;
  }

  if (size < kRdbMagicSize + 1 + 1) {  // magic + version + 至少一个 EOF
    r.error = "RDB file too small";
    return r;
  }
  if (std::memcmp(buf.data(), kRdbMagic, kRdbMagicSize) != 0) {
    r.error = "not a mycache RDB file (bad magic)";
    return r;
  }

  size_t pos = kRdbMagicSize;
  if (buf[pos] != kRdbVersion) {
    r.error = "unsupported RDB version";
    return r;
  }
  ++pos;

  while (pos < size) {
    if (buf[pos] == kRdbEof) {
      ++pos;
      break;
    }
    if (pos + 8 > size) {
      r.error = "truncated record header";
      return r;
    }
    const unsigned char* hdr = buf.data() + pos;
    uint32_t len = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                   (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    uint32_t crc = static_cast<uint32_t>(hdr[4]) | (static_cast<uint32_t>(hdr[5]) << 8) |
                   (static_cast<uint32_t>(hdr[6]) << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
    pos += 8;
    if (pos + len > size) {
      r.error = "truncated record payload";
      return r;
    }
    if (aof::Crc32(buf.data() + pos, len) != crc) {
      r.error = "RDB checksum mismatch";
      return r;
    }
    if (!ApplyRecord(router, buf.data() + pos, len)) {
      r.error = "invalid RDB record";
      return r;
    }
    pos += len;
    ++r.keys_loaded;
  }

  if (pos != size) {
    r.error = "trailing bytes after EOF";
    return r;
  }

  r.ok = true;
  return r;
}

}  // namespace mycache
