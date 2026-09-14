#include "mycache/eviction/lru.h"

#include <atomic>

#include "mycache/common/time.h"

namespace mycache {

namespace {
std::atomic<uint64_t> g_lru_clock{0};
}  // namespace

uint64_t LruClock() {
  uint64_t c = g_lru_clock.load(std::memory_order_relaxed);
  if (c == 0) {
    c = static_cast<uint64_t>(UnixMs());
    g_lru_clock.store(c, std::memory_order_relaxed);
  }
  return c;
}

void RefreshLruClock() {
  g_lru_clock.store(static_cast<uint64_t>(UnixMs()), std::memory_order_relaxed);
}

}  // namespace mycache
