#include "mycache/monitor/info.h"

#include <cstdio>

#include "mycache/common/config.h"
#include "mycache/eviction/eviction_policy.h"
#include "mycache/monitor/metrics.h"
#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/memory/allocator.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace mycache {

namespace {

constexpr const char* kVersion = "0.5.0";

std::string HumanBytes(size_t n) {
  char buf[64];
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  double v = static_cast<double>(n);
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  if (u == 0) {
    std::snprintf(buf, sizeof(buf), "%zuB", n);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f%s", v, units[u]);
  }
  return std::string(buf);
}

// 当前进程 RSS（驻留集），用于计算内存碎片率。Linux 专用，其余平台返回 0。
size_t ResidentMemoryBytes() {
#ifdef __linux__
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long size = 0, resident = 0;
  if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) {
    std::fclose(f);
    return 0;
  }
  std::fclose(f);
  long page = ::sysconf(_SC_PAGESIZE);
  if (page <= 0) page = 4096;
  return static_cast<size_t>(resident) * static_cast<size_t>(page);
#else
  return 0;
#endif
}

}  // namespace

std::string BuildInfo(const Config& config, const CommandRouter& router) {
  size_t used = used_memory();
  size_t rss = ResidentMemoryBytes();
  double frag = (used > 0) ? static_cast<double>(rss) / static_cast<double>(used) : 0.0;

  uint64_t hits = Metrics::Instance().hits();
  uint64_t misses = Metrics::Instance().misses();
  uint64_t lookups = hits + misses;
  double hit_ratio = lookups > 0 ? static_cast<double>(hits) / static_cast<double>(lookups) : 0.0;

  size_t total_keys = 0;
  size_t total_expires = 0;
  for (size_t i = 0; i < router.shard_count(); ++i) {
    Shard* s = router.ShardAt(i);
    total_keys += s->Size();
    total_expires += s->ExpireCount();
  }

  EvictionPolicy p;
  const char* policy_name = ParseEvictionPolicy(config.maxmemory_policy, &p)
                                ? EvictionPolicyName(p)
                                : config.maxmemory_policy.c_str();

  char buf[2048];
  int n = 0;
  n += std::snprintf(buf + n, sizeof(buf) - n, "# Server\r\n");
  n += std::snprintf(buf + n, sizeof(buf) - n, "mycache_version:%s\r\n", kVersion);
  n += std::snprintf(buf + n, sizeof(buf) - n, "uptime_in_seconds:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().UptimeSeconds()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "\r\n# Clients\r\n");
  n += std::snprintf(buf + n, sizeof(buf) - n, "connected_clients:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().connected_clients()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "\r\n# Memory\r\n");
  n += std::snprintf(buf + n, sizeof(buf) - n, "used_memory:%zu\r\n", used);
  n += std::snprintf(buf + n, sizeof(buf) - n, "used_memory_human:%s\r\n", HumanBytes(used).c_str());
  n += std::snprintf(buf + n, sizeof(buf) - n, "maxmemory:%lld\r\n",
                     static_cast<long long>(config.maxmemory));
  n += std::snprintf(buf + n, sizeof(buf) - n, "maxmemory_policy:%s\r\n", policy_name);
  n += std::snprintf(buf + n, sizeof(buf) - n, "mem_fragmentation_ratio:%.2f\r\n", frag);
  n += std::snprintf(buf + n, sizeof(buf) - n, "\r\n# Stats\r\n");
  n += std::snprintf(buf + n, sizeof(buf) - n, "total_connections_received:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().connections()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "total_commands_processed:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().commands()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "keyspace_hits:%llu\r\n",
                     static_cast<unsigned long long>(hits));
  n += std::snprintf(buf + n, sizeof(buf) - n, "keyspace_misses:%llu\r\n",
                     static_cast<unsigned long long>(misses));
  n += std::snprintf(buf + n, sizeof(buf) - n, "keyspace_hits_ratio:%.2f\r\n", hit_ratio);
  n += std::snprintf(buf + n, sizeof(buf) - n, "expired_keys:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().expired()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "evicted_keys:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().evicted()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "stale_replies:%llu\r\n",
                     static_cast<unsigned long long>(Metrics::Instance().stale_replies()));
  n += std::snprintf(buf + n, sizeof(buf) - n, "\r\n# Keyspace\r\n");
  n += std::snprintf(buf + n, sizeof(buf) - n, "db0:keys=%zu,expires=%zu\r\n", total_keys, total_expires);

  return std::string(buf, static_cast<size_t>(n));
}

}  // namespace mycache
