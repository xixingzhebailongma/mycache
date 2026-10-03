// GET/SET 基准测试客户端（单连接，逐条 round-trip，in-flight=1）。
//
// 与 benchmark_pipeline.cpp（深度流水线、只看吞吐上限）互补：本客户端测量真实单条
// 往返延迟，输出 QPS / P50 / P99，用于「并发模型 vs 尾延迟」的对照。
//
// 用法：mycache_get_set_bench [-h host] [-p port] [-n nreq] [-d value_size]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct BenchArgs {
  std::string host = "127.0.0.1";
  int port = 6379;
  int nreq = 50000;  // 每阶段请求数（SET 与 GET 各 nreq 条）
  int dsize = 64;    // value 字节数
};

bool ParseArgs(int argc, char** argv, BenchArgs* a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "-p" || arg == "-n" || arg == "-d") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", arg.c_str());
        return false;
      }
      const char* v = argv[++i];
      if (arg == "-h") a->host = v;
      else if (arg == "-p") a->port = std::atoi(v);
      else if (arg == "-n") a->nreq = std::atoi(v);
      else a->dsize = std::atoi(v);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", arg.c_str());
      return false;
    }
  }
  return a->nreq > 0 && a->dsize > 0;
}

bool SendAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool RecvExact(int fd, size_t n) {
  std::vector<char> buf(64 * 1024);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf.data(), std::min(buf.size(), n - got), 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;  // 连接提前关闭
    got += static_cast<size_t>(r);
  }
  return true;
}

std::string SetCmd(int i, const std::string& value) {
  std::string key = "bench:" + std::to_string(i);
  return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key +
         "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string GetCmd(int i) {
  std::string key = "bench:" + std::to_string(i);
  return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
}

// nearest-rank 法取百分位；入参为已升序的毫秒延迟数组。
double Percentile(const std::vector<double>& sorted_ms, double p) {
  if (sorted_ms.empty()) return 0.0;
  size_t n = sorted_ms.size();
  size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * n));
  if (rank == 0) rank = 1;
  if (rank > n) rank = n;
  return sorted_ms[rank - 1];
}

int Connect(const std::string& host, int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { std::perror("socket"); return -1; }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::fprintf(stderr, "bad host: %s\n", host.c_str());
    ::close(fd);
    return -1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    std::perror("connect");
    ::close(fd);
    return -1;
  }
  return fd;
}

// 对 [0, nreq) 键逐条 round-trip，收集每次延迟(ms)；GET 阶段前必须先预热填充。
bool RunPhase(int fd, int nreq, const std::string& value, bool is_get, double* qps,
              double* p50, double* p99) {
  const size_t reply_len =
      is_get ? (1 + std::to_string(value.size()).size() + 2 + value.size() + 2)
             : 5;  // GET: "$len\r\n" + data + "\r\n"；SET: "+OK\r\n"

  std::vector<double> lat_ms;
  lat_ms.reserve(static_cast<size_t>(nreq));

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < nreq; ++i) {
    std::string cmd = is_get ? GetCmd(i) : SetCmd(i, value);
    auto s = std::chrono::steady_clock::now();
    if (!SendAll(fd, cmd.data(), cmd.size()) || !RecvExact(fd, reply_len)) {
      std::fprintf(stderr, "request %d failed\n", i);
      return false;
    }
    auto e = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(e - s).count());
  }
  const auto t1 = std::chrono::steady_clock::now();

  double sec = std::chrono::duration<double>(t1 - t0).count();
  *qps = sec > 0 ? nreq / sec : 0.0;

  std::sort(lat_ms.begin(), lat_ms.end());
  *p50 = Percentile(lat_ms, 50);
  *p99 = Percentile(lat_ms, 99);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  BenchArgs a;
  if (!ParseArgs(argc, argv, &a)) {
    std::fprintf(stderr,
                 "usage: mycache_get_set_bench [-h host] [-p port] [-n nreq] [-d value_size]\n");
    return 2;
  }

  int fd = Connect(a.host, a.port);
  if (fd < 0) return 1;

  const std::string value(static_cast<size_t>(a.dsize), 'x');

  // 预热：填充全部键，避免 GET 阶段 miss（也便于测量纯读路径）。
  const size_t set_reply = 5;
  for (int i = 0; i < a.nreq; ++i) {
    std::string cmd = SetCmd(i, value);
    if (!SendAll(fd, cmd.data(), cmd.size()) || !RecvExact(fd, set_reply)) {
      std::fprintf(stderr, "warm-up failed at key %d\n", i);
      ::close(fd);
      return 1;
    }
  }

  double qps, p50, p99;
  std::printf("single-connection round-trip benchmark: host=%s port=%d nreq=%d value=%dB\n",
              a.host.c_str(), a.port, a.nreq, a.dsize);

  if (!RunPhase(fd, a.nreq, value, /*is_get=*/false, &qps, &p50, &p99)) {
    ::close(fd);
    return 1;
  }
  std::printf("SET  QPS=%8.0f  P50=%7.3fms  P99=%7.3fms\n", qps, p50, p99);

  if (!RunPhase(fd, a.nreq, value, /*is_get=*/true, &qps, &p50, &p99)) {
    ::close(fd);
    return 1;
  }
  std::printf("GET  QPS=%8.0f  P50=%7.3fms  P99=%7.3fms\n", qps, p50, p99);

  ::close(fd);
  return 0;
}
