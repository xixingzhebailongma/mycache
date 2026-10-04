#include "mycache/storage/memory/memory_pool.h"

#include <cstdlib>

namespace mycache {
namespace {

// 前缀头 16 字节。必须是 16 的倍数：base 16 对齐 => user = base + 16 仍 16 对齐。
// 不能是 24（base + 24 ≡ 8 mod 16，破坏 SSE / long double 对齐）。
struct PoolHeader {
  uint64_t magic;       // 8B 魔数
  uint32_t size_class;  // 4B 所属 class（16..4096）
  uint32_t flags;       // 4B 保留
};
static_assert(sizeof(PoolHeader) == 16, "PoolHeader must be 16 bytes");

constexpr uint64_t kMagic = 0x5A4D414C4C4F4331ULL;  // "ZMALLOC1"

// size class 表：全为 16 的倍数，min=16（8 配 16B 头 payload 效率仅 8/24=33%，砍掉）。
constexpr uint32_t kSizeClasses[] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
};
constexpr uint32_t kNumClasses = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);

constexpr uint32_t kMaxBlocksPerClass = 1024;         // 单 class freelist 深度上限
constexpr uint32_t kClassBudgetBytes = 64 * 1024;     // 64 KiB：单 class 空闲字节预算
constexpr size_t kBudgetPerThread = 8 * 1024 * 1024;  // 每线程总硬顶（计 block = class + 16）

// 池开关：进程内读一次环境变量，A/B 用同一二进制运行时切换。
const bool kPoolDisabled = [] {
  const char* e = std::getenv("MYCACHE_DISABLE_POOL");
  return e && *e && *e != '0';
}();

// 空闲块复用其用户区（class >= 16，放得下一个指针）做侵入式单链。
struct FreeNode { FreeNode* next; };

// POD、无析构：靠显式 PoolFlushAll()，不依赖 thread_local 析构（静态对象析构期
// 仍可能调 zfree 会 crash）。
struct PoolState {
  FreeNode* free[kNumClasses] = {};
  uint32_t count[kNumClasses] = {};
  size_t retained_bytes = 0;
};
thread_local PoolState t_pool;

inline PoolHeader* HeaderOf(void* user) {
  return reinterpret_cast<PoolHeader*>(static_cast<char*>(user) - sizeof(PoolHeader));
}
inline const PoolHeader* HeaderOf(const void* user) {
  return reinterpret_cast<const PoolHeader*>(static_cast<const char*>(user) - sizeof(PoolHeader));
}
inline void* BaseOf(void* user) {
  return static_cast<char*>(user) - sizeof(PoolHeader);
}

uint32_t ClassFor(size_t size) {  // 最小 class >= size（调用方保证 1 <= size <= 4096）
  for (uint32_t c : kSizeClasses) {
    if (size <= c) return c;
  }
  return kMaxPoolSize;  // unreachable
}

uint32_t ClassIndex(uint32_t cls) {
  for (uint32_t i = 0; i < kNumClasses; ++i) {
    if (kSizeClasses[i] == cls) return i;
  }
  return kNumClasses;  // 非法
}

uint32_t CapBlocks(uint32_t cls) {
  uint32_t by_budget = kClassBudgetBytes / cls;
  return by_budget < kMaxBlocksPerClass ? by_budget : kMaxBlocksPerClass;
}

}  // namespace

bool PoolDisabled() { return kPoolDisabled; }

void* PoolAlloc(size_t size, uint32_t* out_class) {
  uint32_t cls = ClassFor(size);
  uint32_t ci = ClassIndex(cls);
  if (out_class) *out_class = cls;

  FreeNode* node = t_pool.free[ci];
  if (node) {  // 命中空闲块：pop（头仍是首次 malloc 时写入的 magic + class）
    t_pool.free[ci] = node->next;
    --t_pool.count[ci];
    t_pool.retained_bytes -= (cls + sizeof(PoolHeader));
    return node;
  }

  void* base = std::malloc(cls + sizeof(PoolHeader));  // 未命中：向系统要一块
  if (!base) return nullptr;
  auto* h = static_cast<PoolHeader*>(base);
  h->magic = kMagic;
  h->size_class = cls;
  h->flags = 0;
  return static_cast<char*>(base) + sizeof(PoolHeader);
}

void PoolFree(void* ptr) {
  uint32_t cls = HeaderOf(ptr)->size_class;
  uint32_t ci = ClassIndex(cls);
  if (ci >= kNumClasses) {  // 防御：非法 class（不应发生）
    std::free(BaseOf(ptr));
    return;
  }
  // 压栈：不超 per-class cap 且不超 per-thread 总字节预算；否则裸 free 归还 OS。
  if (t_pool.count[ci] < CapBlocks(cls) &&
      t_pool.retained_bytes + (cls + sizeof(PoolHeader)) <= kBudgetPerThread) {
    auto* node = reinterpret_cast<FreeNode*>(ptr);  // 复用用户区存 next 指针
    node->next = t_pool.free[ci];
    t_pool.free[ci] = node;
    ++t_pool.count[ci];
    t_pool.retained_bytes += (cls + sizeof(PoolHeader));
  } else {
    std::free(BaseOf(ptr));
  }
}

bool PoolOwns(const void* ptr, uint32_t* out_class) {
  // 读 ptr[-16..-1] 对真实 malloc 块是技术性 UB（glibc 下可读不崩）；命中是相邻
  // chunk 用户数据的结构化碰撞，概率极低但不为零，故再做 class 区间校验，失败一律
  // 按真实块走 free()，绝不 crash。
  const PoolHeader* h = HeaderOf(ptr);
  if (h->magic != kMagic) return false;
  if (h->size_class > kMaxPoolSize) return false;
  if (ClassIndex(h->size_class) >= kNumClasses) return false;
  if (out_class) *out_class = h->size_class;
  return true;
}

void PoolFlushAll() {
  for (uint32_t ci = 0; ci < kNumClasses; ++ci) {
    FreeNode* node = t_pool.free[ci];
    while (node) {
      FreeNode* next = node->next;
      std::free(BaseOf(node));
      node = next;
    }
    t_pool.free[ci] = nullptr;
    t_pool.count[ci] = 0;
  }
  t_pool.retained_bytes = 0;
}

}  // namespace mycache
