#pragma once

#include <chrono>
#include <cstdint>

namespace mycache {

inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 墙钟时间（毫秒，Unix 纪元）。用于 TTL 到期时间戳：与 steady_clock 不同，
// 该时钟在进程重启后仍然一致，因此写入 AOF 的绝对到期时间（PEXPIREAT）可被
// 可靠地重放。等价于 Redis 的 mstime()。
inline int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace mycache
