#include "mycache/monitor/metrics.h"

#include "mycache/common/time.h"

namespace mycache {

Metrics& Metrics::Instance() {
  static Metrics m;
  return m;
}

void Metrics::MarkStart() {
  int64_t expected = 0;
  start_ms_.compare_exchange_strong(expected, NowMs());
}

uint64_t Metrics::UptimeSeconds() const {
  int64_t s = start_ms_.load(std::memory_order_relaxed);
  if (s == 0) return 0;
  return static_cast<uint64_t>((NowMs() - s) / 1000);
}

}  // namespace mycache
