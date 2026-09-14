#pragma once

#include <cstddef>
#include <cstdint>

namespace mycache {

// SDS (Simple Dynamic String)：带长度前缀的二进制安全字符串。
//
// 返回给使用方的 `sds` 是指向数据缓冲区的 char*（与 C 字符串兼容，始终以
// '\0' 结尾，但内容可以包含 '\0'）。真正的元数据存放在紧邻数据之前的头部
// （sdshdr8/16/32/64），通过 `s[-1]` 的 flags 字节反推头部大小与类型。
//
// 布局（以 sdshdr8 为例）：
//   +----------+---------------+--------+
//   | sdshdr8  | len 字节数据   | '\0'   |
//   +----------+---------------+--------+
//   ^          ^
//   |          +-- sds（返回给用户的指针；s[-1] 即 flags）
//   +-- 头部（通过 s - sizeof(sdshdrN) 反推）
//
// `alloc` 表示缓冲区可用容量（不含结尾 '\0'），因此 avail = alloc - len。
// 追加时若 avail 不足，会按 2 倍（超过 1MB 后改为每次 +1MB）预分配，以摊薄
// realloc 次数。取长度是 O(1)，比较是二进制安全的。

using sds = char*;

// 头部类型标志（存于 flags 低 3 位）。
enum : uint8_t {
  SDS_TYPE_8 = 0,
  SDS_TYPE_16 = 1,
  SDS_TYPE_32 = 2,
  SDS_TYPE_64 = 3,
  SDS_TYPE_MASK = 7,
};

#if defined(__GNUC__) || defined(__clang__)
#define SDS_PACKED __attribute__((__packed__))
#else
#define SDS_PACKED
#endif

// 使用 packed 是为了让 flags 字节始终紧邻数据缓冲区之前（即 s[-1]），
// 与 Redis 的 sds 头部布局一致。注意 packed 属性须置于 struct 之后、标签之前。
struct SDS_PACKED sdshdr8 { uint8_t len; uint8_t alloc; uint8_t flags; };
struct SDS_PACKED sdshdr16 { uint16_t len; uint16_t alloc; uint8_t flags; };
struct SDS_PACKED sdshdr32 { uint32_t len; uint32_t alloc; uint8_t flags; };
struct SDS_PACKED sdshdr64 { uint64_t len; uint64_t alloc; uint8_t flags; };

#undef SDS_PACKED

// 给定字符串长度，选择最小的头部类型。
inline uint8_t sdsReqType(size_t size) {
  if (size < (1ull << 8)) return SDS_TYPE_8;
  if (size < (1ull << 16)) return SDS_TYPE_16;
  if (size < (1ull << 32)) return SDS_TYPE_32;
  return SDS_TYPE_64;
}

inline size_t sdsHdrSize(uint8_t type) {
  switch (type & SDS_TYPE_MASK) {
    case SDS_TYPE_8: return sizeof(sdshdr8);
    case SDS_TYPE_16: return sizeof(sdshdr16);
    case SDS_TYPE_32: return sizeof(sdshdr32);
    case SDS_TYPE_64: return sizeof(sdshdr64);
  }
  return 0;
}

// ---- 内联只读访问器（接受 const char*，避免无谓的 const_cast）----

inline size_t sdsHdrSize(const char* s) {
  return sdsHdrSize(static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK);
}

inline size_t sdslen(const char* s) {
  switch (static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK) {
    case SDS_TYPE_8: return reinterpret_cast<const sdshdr8*>(s - sizeof(sdshdr8))->len;
    case SDS_TYPE_16: return reinterpret_cast<const sdshdr16*>(s - sizeof(sdshdr16))->len;
    case SDS_TYPE_32: return reinterpret_cast<const sdshdr32*>(s - sizeof(sdshdr32))->len;
    case SDS_TYPE_64: return reinterpret_cast<const sdshdr64*>(s - sizeof(sdshdr64))->len;
  }
  return 0;
}

inline size_t sdsalloc(const char* s) {
  switch (static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK) {
    case SDS_TYPE_8: return reinterpret_cast<const sdshdr8*>(s - sizeof(sdshdr8))->alloc;
    case SDS_TYPE_16: return reinterpret_cast<const sdshdr16*>(s - sizeof(sdshdr16))->alloc;
    case SDS_TYPE_32: return reinterpret_cast<const sdshdr32*>(s - sizeof(sdshdr32))->alloc;
    case SDS_TYPE_64: return reinterpret_cast<const sdshdr64*>(s - sizeof(sdshdr64))->alloc;
  }
  return 0;
}

inline size_t sdsavail(const char* s) { return sdsalloc(s) - sdslen(s); }

// 该 SDS 占用的总内存（头部 + 容量 + 结尾 '\0'）。
inline size_t sdsAllocSize(const char* s) { return sdsHdrSize(s) + sdsalloc(s) + 1; }

// ---- 内联写访问器（仅供内部使用）----

inline void sdssetlen(sds s, size_t newlen) {
  switch (static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK) {
    case SDS_TYPE_8: reinterpret_cast<sdshdr8*>(s - sizeof(sdshdr8))->len = static_cast<uint8_t>(newlen); break;
    case SDS_TYPE_16: reinterpret_cast<sdshdr16*>(s - sizeof(sdshdr16))->len = static_cast<uint16_t>(newlen); break;
    case SDS_TYPE_32: reinterpret_cast<sdshdr32*>(s - sizeof(sdshdr32))->len = static_cast<uint32_t>(newlen); break;
    case SDS_TYPE_64: reinterpret_cast<sdshdr64*>(s - sizeof(sdshdr64))->len = static_cast<uint64_t>(newlen); break;
  }
}

inline void sdssetalloc(sds s, size_t newalloc) {
  switch (static_cast<uint8_t>(s[-1]) & SDS_TYPE_MASK) {
    case SDS_TYPE_8: reinterpret_cast<sdshdr8*>(s - sizeof(sdshdr8))->alloc = static_cast<uint8_t>(newalloc); break;
    case SDS_TYPE_16: reinterpret_cast<sdshdr16*>(s - sizeof(sdshdr16))->alloc = static_cast<uint16_t>(newalloc); break;
    case SDS_TYPE_32: reinterpret_cast<sdshdr32*>(s - sizeof(sdshdr32))->alloc = static_cast<uint32_t>(newalloc); break;
    case SDS_TYPE_64: reinterpret_cast<sdshdr64*>(s - sizeof(sdshdr64))->alloc = static_cast<uint64_t>(newalloc); break;
  }
}

// 初始化头部（len/alloc/flags）。sds 头部紧跟数据之前，创建时据此写回元数据。
inline void sdsSetHeader(sds s, size_t len, size_t alloc, uint8_t type) {
  switch (type) {
    case SDS_TYPE_8: { auto* h = reinterpret_cast<sdshdr8*>(s - sizeof(sdshdr8)); h->len = static_cast<uint8_t>(len); h->alloc = static_cast<uint8_t>(alloc); h->flags = type; break; }
    case SDS_TYPE_16: { auto* h = reinterpret_cast<sdshdr16*>(s - sizeof(sdshdr16)); h->len = static_cast<uint16_t>(len); h->alloc = static_cast<uint16_t>(alloc); h->flags = type; break; }
    case SDS_TYPE_32: { auto* h = reinterpret_cast<sdshdr32*>(s - sizeof(sdshdr32)); h->len = static_cast<uint32_t>(len); h->alloc = static_cast<uint32_t>(alloc); h->flags = type; break; }
    case SDS_TYPE_64: { auto* h = reinterpret_cast<sdshdr64*>(s - sizeof(sdshdr64)); h->len = static_cast<uint64_t>(len); h->alloc = static_cast<uint64_t>(alloc); h->flags = type; break; }
  }
}

// ---- 非内联操作（定义见 src/storage/string/sds.cpp）----

sds sdsnewlen(const void* init, size_t initlen);
sds sdsnew(const char* init);
sds sdsempty();
void sdsfree(sds s);

sds sdsdup(const char* s);
void sdsclear(sds s);

sds sdsMakeRoomFor(sds s, size_t addlen);
sds sdscatlen(sds s, const void* t, size_t len);
sds sdscat(sds s, const char* t);
sds sdscpylen(sds s, const char* t, size_t len);
sds sdscpy(sds s, const char* t);

int sdscmp(const char* s1, const char* s2);

sds sdsfromlonglong(long long value);

}  // namespace mycache
