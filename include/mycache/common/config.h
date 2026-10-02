#pragma once

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace mycache {

// 解析内存大小字符串，支持 Redis 风格后缀：k/m/g 为 1000 进制，kb/mb/gb 为
// 1024 进制（与 Redis 的 memtoll 约定一致）。纯数字按字节。失败返回 false。
inline bool ParseMemorySize(const std::string& s, int64_t* out) {
  if (s.empty() || !out) return false;
  size_t i = 0;
  if (s[i] == '-') return false;

  uint64_t n = 0;
  bool digits = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    n = n * 10 + static_cast<uint64_t>(s[i] - '0');
    digits = true;
    ++i;
  }
  if (!digits) return false;

  std::string unit = s.substr(i);
  for (auto& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  uint64_t mult = 1;
  if (unit.empty() || unit == "b") mult = 1;
  else if (unit == "k") mult = 1000ull;
  else if (unit == "kb") mult = 1024ull;
  else if (unit == "m") mult = 1000ull * 1000ull;
  else if (unit == "mb") mult = 1024ull * 1024ull;
  else if (unit == "g") mult = 1000ull * 1000ull * 1000ull;
  else if (unit == "gb") mult = 1024ull * 1024ull * 1024ull;
  else return false;

  if (n > static_cast<uint64_t>(INT64_MAX) / mult) return false;
  *out = static_cast<int64_t>(n * mult);
  return true;
}

// 运行时配置，从简单的 "key value" 文件解析而来。
struct Config {
  int port = 6379;
  std::string bind = "0.0.0.0";
  int io_threads = 4;
  int shard_count = 8;
  int tcp_backlog = 511;
  int max_connections = 10000;
  std::string log_level = "info";
  std::string log_file;

  // ---- AOF 持久化 ----
  bool appendonly = false;              // 是否开启 AOF
  std::string appendfilename = "appendonly.aof";
  std::string appendfsync = "everysec"; // always / everysec / no
  std::string dir;                      // AOF 文件目录；空或 "." 表示当前目录
  int64_t aof_rewrite_min_size = 64 * 1024 * 1024;  // 触发自动 rewrite 的最小 AOF 大小（默认 64mb）
  int     aof_rewrite_percentage = 100;             // 相对上次 rewrite 的增长百分比；<=0 关闭自动 rewrite

  // ---- 内存治理 (Week 5) ----
  int64_t maxmemory = 0;                 // 内存上限（字节）；0 = 不限制
  std::string maxmemory_policy = "noeviction";  // noeviction/allkeys-lru/allkeys-random/volatile-lru/volatile-random
  int maxmemory_samples = 5;             // 每次淘汰时的采样数

  // ---- RDB 持久化 (Week 6) ----
  std::string dbfilename = "dump.rdb";   // SAVE/BGSAVE 快照文件名

  // ---- IO 引擎 (Week 6) ----
  std::string io_engine = "posix";       // posix / io_uring（后者需 liburing）

  // AOF 文件的完整路径（dir + appendfilename）。
  std::string AofPath() const {
    if (dir.empty() || dir == ".") return appendfilename;
    return dir + "/" + appendfilename;
  }

  // RDB 文件的完整路径（dir + dbfilename）。
  std::string RdbPath() const {
    if (dir.empty() || dir == ".") return dbfilename;
    return dir + "/" + dbfilename;
  }

  bool Load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
      size_t hash = line.find('#');
      if (hash != std::string::npos) line = line.substr(0, hash);
      std::istringstream ss(line);
      std::string key, value;
      if (!(ss >> key)) continue;
      if (!(ss >> value)) continue;
      Apply(key, value);
    }
    return true;
  }

  void Apply(const std::string& key, const std::string& value) {
    if (key == "port") port = std::atoi(value.c_str());
    else if (key == "bind") bind = value;
    else if (key == "io_threads") io_threads = std::atoi(value.c_str());
    else if (key == "shard_count") shard_count = std::atoi(value.c_str());
    else if (key == "tcp_backlog") tcp_backlog = std::atoi(value.c_str());
    else if (key == "max_connections") max_connections = std::atoi(value.c_str());
    else if (key == "log_level") log_level = value;
    else if (key == "log_file") log_file = value;
    else if (key == "appendonly") appendonly = (value == "yes" || value == "true" || value == "1");
    else if (key == "appendfilename") appendfilename = value;
    else if (key == "appendfsync") appendfsync = value;
    else if (key == "dir") dir = value;
    else if (key == "maxmemory") { int64_t v = 0; if (ParseMemorySize(value, &v)) maxmemory = v; }
    else if (key == "maxmemory_policy") maxmemory_policy = value;
    else if (key == "maxmemory_samples") maxmemory_samples = std::atoi(value.c_str());
    else if (key == "dbfilename") dbfilename = value;
    else if (key == "io_engine") io_engine = value;
    else if (key == "auto-aof-rewrite-min-size") { int64_t v = 0; if (ParseMemorySize(value, &v)) aof_rewrite_min_size = v; }
    else if (key == "auto-aof-rewrite-percentage") {
      // 只拒绝非数字/越界；负值保留（由 ShouldAutoRewrite 的 percentage<=0 处理"关闭"）。
      char* end = nullptr; errno = 0;
      long v = std::strtol(value.c_str(), &end, 10);
      if (end != value.c_str() && *end == '\0' && errno != ERANGE &&
          v >= INT_MIN && v <= INT_MAX)
        aof_rewrite_percentage = static_cast<int>(v);
    }
  }
};

}  // namespace mycache
