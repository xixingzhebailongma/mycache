#include "mycache/storage/object/string_object.h"

#include <climits>

#include "mycache/eviction/lru.h"
#include "mycache/storage/memory/allocator.h"
#include "mycache/storage/memory/memory_stats.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

bool StringToLongLong(const char* s, size_t slen, long long* out) {
  const char* p = s;
  size_t plen = 0;
  bool negative = false;
  unsigned long long v = 0;

  if (slen == 0) return false;
  if (p[0] == '-') {
    negative = true;
    p++;
    plen++;
    if (plen == slen) return false;  // 单独的 "-"
  }

  if (p[0] >= '1' && p[0] <= '9') {
    v = static_cast<unsigned long long>(p[0] - '0');
    p++;
    plen++;
  } else if (p[0] == '0' && slen == 1) {
    // 单独的 "0"（"-0" 因 slen==2 而被拒绝）
    *out = 0;
    return true;
  } else {
    return false;  // 前导零 / '+' / 其他非法字符
  }

  while (plen < slen && p[0] >= '0' && p[0] <= '9') {
    if (v > (ULLONG_MAX / 10)) return false;
    v *= 10;
    unsigned long long d = static_cast<unsigned long long>(p[0] - '0');
    if (v > (ULLONG_MAX - d)) return false;
    v += d;
    p++;
    plen++;
  }
  if (plen < slen) return false;  // 存在非数字字符

  const unsigned long long kLlongMinMagnitude = 0x8000000000000000ULL;
  if (negative) {
    if (v > kLlongMinMagnitude) return false;
    *out = (v == kLlongMinMagnitude) ? LLONG_MIN : -static_cast<long long>(v);
  } else {
    if (v > static_cast<unsigned long long>(LLONG_MAX)) return false;
    *out = static_cast<long long>(v);
  }
  return true;
}

Object* CreateStringObject(const char* buf, size_t len) {
  sds s = sdsnewlen(buf, len);
  if (!s) return nullptr;
  Object* o = static_cast<Object*>(zmalloc(sizeof(Object)));
  if (!o) {
    sdsfree(s);
    return nullptr;
  }
  o->type = ObjType::kString;
  o->encoding = ObjEncoding::kString;
  o->refcount = 1;
  o->ptr = s;
  o->lru = LruClock();
  MemoryStats::Instance().IncrString(ObjEncoding::kString);
  return o;
}

Object* CreateIntObject(long long value) {
  Object* o = static_cast<Object*>(zmalloc(sizeof(Object)));
  if (!o) return nullptr;
  o->type = ObjType::kString;
  o->encoding = ObjEncoding::kInt;
  o->refcount = 1;
  o->ptr = LongLongToObjectPtr(value);
  o->lru = LruClock();
  MemoryStats::Instance().IncrString(ObjEncoding::kInt);
  return o;
}

Object* TryObjectEncoding(Object* o) {
  if (!o->IsString() || o->encoding != ObjEncoding::kString) return o;
  sds s = static_cast<sds>(o->ptr);
  size_t len = sdslen(s);
  if (len <= 20) {  // long long 的十进制表示最多 20 字节
    long long value;
    if (StringToLongLong(s, len, &value)) {
      // 释放独立 sds，原地改写为 int 编码。
      sdsfree(s);
      MemoryStats::Instance().DecrString(ObjEncoding::kString);
      o->encoding = ObjEncoding::kInt;
      o->ptr = LongLongToObjectPtr(value);
      MemoryStats::Instance().IncrString(ObjEncoding::kInt);
    }
  }
  return o;
}

}  // namespace mycache
