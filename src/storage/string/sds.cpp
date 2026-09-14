#include "mycache/storage/string/sds.h"

#include <cstdio>
#include <cstring>

#include "mycache/storage/memory/allocator.h"

namespace mycache {

namespace {
constexpr size_t kSdsMaxPrealloc = 1024 * 1024;  // 1MB
constexpr int kLlongStrSize = 21;  // 足以容纳 "-9223372036854775808\0"
}  // namespace

sds sdsnewlen(const void* init, size_t initlen) {
  uint8_t type = sdsReqType(initlen);
  size_t hdrlen = sdsHdrSize(type);
  unsigned char* sh = static_cast<unsigned char*>(zmalloc(hdrlen + initlen + 1));
  if (!sh) return nullptr;
  sds s = reinterpret_cast<sds>(sh + hdrlen);
  if (init && initlen) std::memcpy(s, init, initlen);
  s[initlen] = '\0';
  sdsSetHeader(s, initlen, initlen, type);
  return s;
}

sds sdsnew(const char* init) {
  size_t len = init ? std::strlen(init) : 0;
  return sdsnewlen(init, len);
}

sds sdsempty() { return sdsnewlen("", 0); }

void sdsfree(sds s) {
  if (!s) return;
  zfree(reinterpret_cast<void*>(s - sdsHdrSize(s)));
}

sds sdsdup(const char* s) {
  return sdsnewlen(s, sdslen(s));
}

void sdsclear(sds s) {
  sdssetlen(s, 0);
  s[0] = '\0';
}

sds sdsMakeRoomFor(sds s, size_t addlen) {
  size_t len = sdslen(s);
  size_t avail = sdsavail(s);
  if (avail >= addlen) return s;

  size_t newlen = len + addlen;
  if (newlen < kSdsMaxPrealloc) newlen *= 2;
  else newlen += kSdsMaxPrealloc;

  uint8_t old_type = static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK;
  uint8_t type = sdsReqType(newlen);

  if (type == old_type) {
    // 头部类型不变：原地 realloc。
    size_t hdrlen = sdsHdrSize(type);
    unsigned char* sh = static_cast<unsigned char*>(
        zrealloc(reinterpret_cast<void*>(s - hdrlen), hdrlen + newlen + 1));
    if (!sh) return nullptr;
    s = reinterpret_cast<sds>(sh + hdrlen);
    sdssetalloc(s, newlen);
    return s;
  }

  // 头部类型升级：分配新块并搬移数据。
  size_t new_hdrlen = sdsHdrSize(type);
  unsigned char* sh = static_cast<unsigned char*>(zmalloc(new_hdrlen + newlen + 1));
  if (!sh) return nullptr;
  sds new_s = reinterpret_cast<sds>(sh + new_hdrlen);
  std::memcpy(new_s, s, len + 1);
  zfree(reinterpret_cast<void*>(s - sdsHdrSize(old_type)));
  sdsSetHeader(new_s, len, newlen, type);
  return new_s;
}

sds sdscatlen(sds s, const void* t, size_t len) {
  size_t curlen = sdslen(s);
  s = sdsMakeRoomFor(s, len);
  if (!s) return nullptr;
  std::memcpy(s + curlen, t, len);
  sdssetlen(s, curlen + len);
  s[curlen + len] = '\0';
  return s;
}

sds sdscat(sds s, const char* t) {
  return sdscatlen(s, t, std::strlen(t));
}

sds sdscpylen(sds s, const char* t, size_t len) {
  if (sdsalloc(s) < len) {
    s = sdsMakeRoomFor(s, len - sdslen(s));
    if (!s) return nullptr;
  }
  std::memcpy(s, t, len);
  s[len] = '\0';
  sdssetlen(s, len);
  return s;
}

sds sdscpy(sds s, const char* t) {
  return sdscpylen(s, t, std::strlen(t));
}

int sdscmp(const char* s1, const char* s2) {
  size_t l1 = sdslen(s1);
  size_t l2 = sdslen(s2);
  size_t minlen = l1 < l2 ? l1 : l2;
  int cmp = std::memcmp(s1, s2, minlen);
  if (cmp == 0) return l1 < l2 ? -1 : (l1 > l2 ? 1 : 0);
  return cmp;
}

sds sdsfromlonglong(long long value) {
  char buf[kLlongStrSize];
  int len = std::snprintf(buf, sizeof(buf), "%lld", value);
  return sdsnewlen(buf, static_cast<size_t>(len));
}

}  // namespace mycache
