#include "mycache/eviction/eviction_policy.h"

namespace mycache {

bool AllowSharedIntegers(const EvictionConfig& ec) {
  switch (ec.policy) {
    case EvictionPolicy::kAllkeysLru:
    case EvictionPolicy::kVolatileLru:
      return false;  // LRU 依赖每对象独立 lru，共享会破坏淘汰
    default:
      return true;  // noeviction / random 不依赖逐 key lru
  }
}

bool ParseEvictionPolicy(const std::string& s, EvictionPolicy* out) {
  if (!out) return false;
  if (s == "noeviction") *out = EvictionPolicy::kNoEviction;
  else if (s == "allkeys-lru") *out = EvictionPolicy::kAllkeysLru;
  else if (s == "allkeys-random") *out = EvictionPolicy::kAllkeysRandom;
  else if (s == "volatile-lru") *out = EvictionPolicy::kVolatileLru;
  else if (s == "volatile-random") *out = EvictionPolicy::kVolatileRandom;
  else return false;
  return true;
}

const char* EvictionPolicyName(EvictionPolicy p) {
  switch (p) {
    case EvictionPolicy::kNoEviction: return "noeviction";
    case EvictionPolicy::kAllkeysLru: return "allkeys-lru";
    case EvictionPolicy::kAllkeysRandom: return "allkeys-random";
    case EvictionPolicy::kVolatileLru: return "volatile-lru";
    case EvictionPolicy::kVolatileRandom: return "volatile-random";
  }
  return "unknown";
}

}  // namespace mycache
