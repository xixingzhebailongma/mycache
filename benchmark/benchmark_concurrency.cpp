// 多连接、多线程并发基准测试客户端。每个工作线程持有若干条连接，逐条 round-trip
// （每连接 in-flight=1，与 redis-benchmark 默认行为一致），分 SET / GET 两阶段，
// 输出跨全部连接/线程聚合的 QPS + P50/P99。
//
// 用法：mycache_concurrency_bench [-h host] [-p port] [-c conns] [-t threads]
//                                 [-n per_conn] [-d value_size]
//
// 语义：-c 连接总数，-t 工作线程数（默认 = -c），-n 每连接每阶段请求数（总请求 = c*n）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchArgs {
  std::string host = "127.0.0.1";
  int port = 6379;
  int conns = 50;      // 连接总数
  int threads = 0;     // 工作线程数（0 = 自动取 conns）
  int per_conn = 2000; // 每连接每阶段请求数
  int dsize = 64;      // value 字节数
};

bool ParseArgs(int argc, char** argv, BenchArgs* a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "-p" || arg == "-c" || arg == "-t" || arg == "-n" ||
        arg == "-d") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", arg.c_str());
        return false;
      }
      const char* v = argv[++i];
      if (arg == "-h") a->host = v;
      else if (arg == "-p") a->port = std::atoi(v);
      else if (arg == "-c") a->conns = std::atoi(v);
      else if (arg == "-t") a->threads = std::atoi(v);
      else if (arg == "-n") a->per_conn = std::atoi(v);
      else a->dsize = std::atoi(v);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", arg.c_str());
      return false;
    }
  }
  if (a->threads <= 0) a->threads = a->conns;
  if (a->threads > a->conns) a->threads = a->conns;
  return a->conns > 0 && a->per_conn > 0 && a->dsize > 0;
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
    if (r == 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

std::string SetCmd(int key_id, const std::string& value) {
  std::string key = "bench:" + std::to_string(key_id);
  return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key +
         "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string GetCmd(int key_id) {
  std::string key = "bench:" + std::to_string(key_id);
  return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
}

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

// 单个工作线程：建立 [lo, hi) 范围内的连接，预热后 spin 等待 go，逐条 round-trip，
// 把每次延迟（毫秒）写进自己的槽位。测量阶段外（预热）不记时。
void Worker(const BenchArgs& a, int lo, int hi, bool is_get, bool measure,
            std::atomic<bool>& go, std::atomic<bool>& fail, std::vector<double>* lat) {
  const std::string value(static_cast<size_t>(a.dsize), 'x');
  const size_t set_reply = 5;
  const size_t get_reply = 1 + std::to_string(value.size()).size() + 2 + value.size() + 2;

  std::vector<int> fds;
  fds.reserve(static_cast<size_t>(hi - lo));
  for (int ci = lo; ci < hi; ++ci) {
    int fd = Connect(a.host, a.port);
    if (fd < 0) { fail.store(true); return; }
    fds.push_back(fd);
  }

  while (!go.load(std::memory_order_acquire)) {
  }

  size_t idx = 0;
  for (int ci = lo; ci < hi; ++ci) {
    int fd = fds[idx++];
    for (int i = 0; i < a.per_conn; ++i) {
      int key_id = ci * a.per_conn + i;
      std::string cmd = is_get ? GetCmd(key_id) : SetCmd(key_id, value);
      size_t reply = is_get ? get_reply : set_reply;
      auto s = std::chrono::steady_clock::now();
      if (!SendAll(fd, cmd.data(), cmd.size()) || !RecvExact(fd, reply)) {
        fail.store(true);
        break;
      }
      auto e = std::chrono::steady_clock::now();
      if (measure) {
        lat->push_back(std::chrono::duration<double, std::milli>(e - s).count());
      }
    }
    ::close(fd);
  }
}

// 并发跑一个阶段（warm-up / SET / GET）。measure=false 时不计时、不产出分位数。
bool ConcurrentPhase(const BenchArgs& a, bool is_get, bool measure, double* qps, double* p50,
                     double* p99, int* total) {
  std::vector<std::thread> pool;
  std::vector<std::vector<double>> slots(static_cast<size_t>(a.threads));
  std::atomic<bool> go{false};
  std::atomic<bool> fail{false};

  for (int tid = 0; tid < a.threads; ++tid) {
    int lo = tid * a.conns / a.threads;
    int hi = (tid + 1) * a.conns / a.threads;
    pool.emplace_back([&, lo, hi, tid]() {
      Worker(a, lo, hi, is_get, measure, go, fail, &slots[tid]);
    });
  }

  auto start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : pool) t.join();
  auto end = std::chrono::steady_clock::now();
  if (fail.load()) return false;

  *total = a.conns * a.per_conn;
  if (measure) {
    double sec = std::chrono::duration<double>(end - start).count();
    *qps = sec > 0 ? *total / sec : 0.0;
    std::vector<double> all;
    for (auto& s : slots) all.insert(all.end(), s.begin(), s.end());
    std::sort(all.begin(), all.end());
    *p50 = Percentile(all, 50);
    *p99 = Percentile(all, 99);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  BenchArgs a;
  if (!ParseArgs(argc, argv, &a)) {
    std::fprintf(stderr,
                 "usage: mycache_concurrency_bench [-h host] [-p port] [-c conns] "
                 "[-t threads] [-n per_conn] [-d value_size]\n");
    return 2;
  }

  double qps, p50, p99;
  int total;

  std::printf("concurrency benchmark: host=%s port=%d conns=%d threads=%d "
              "per_conn=%d value=%dB (total per phase=%d)\n",
              a.host.c_str(), a.port, a.conns, a.threads, a.per_conn, a.dsize,
              a.conns * a.per_conn);

  // 预热：填充全部键（不计时）。
  if (!ConcurrentPhase(a, /*is_get=*/false, /*measure=*/false, &qps, &p50, &p99, &total)) {
    std::fprintf(stderr, "warm-up failed\n");
    return 1;
  }

  // SET 阶段（覆盖写，计时）。
  if (!ConcurrentPhase(a, /*is_get=*/false, /*measure=*/true, &qps, &p50, &p99, &total)) {
    std::fprintf(stderr, "SET phase failed\n");
    return 1;
  }
  std::printf("SET  QPS=%8.0f  P50=%7.3fms  P99=%7.3fms  (n=%d)\n", qps, p50, p99, total);

  // GET 阶段（读回，计时）。
  if (!ConcurrentPhase(a, /*is_get=*/true, /*measure=*/true, &qps, &p50, &p99, &total)) {
    std::fprintf(stderr, "GET phase failed\n");
    return 1;
  }
  std::printf("GET  QPS=%8.0f  P50=%7.3fms  P99=%7.3fms  (n=%d)\n", qps, p50, p99, total);

  return 0;
}
