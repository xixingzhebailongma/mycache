// 深度流水线式 SET/GET 基准测试客户端（迁移自 benchmark/pipeline_test.cpp，行为等价，
// 仅改名以符合 Upgrade 1 的文件命名）。一次性批量发送全部 N 条命令（in-flight=N），
// 读回所有回复，统计吞吐上限。
//
// 注意：深度流水线下吞吐反映「批提交上限」，不含单条往返延迟；延迟见
// benchmark_get_set.cpp（in-flight=1 round-trip）与 benchmark_concurrency.cpp。
//
// 用法：mycache_pipeline_bench [host] [port] [nreq]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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

bool RecvAll(int fd, size_t total) {
  std::vector<char> buf(64 * 1024);
  size_t received = 0;
  while (received < total) {
    ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;  // 连接提前关闭
    received += static_cast<size_t>(n);
  }
  return true;
}

std::string SetCmd(int i, const std::string& value) {
  std::string key = std::to_string(i);
  return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key +
         "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string GetCmd(int i) {
  std::string key = std::to_string(i);
  return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  int port = argc > 2 ? std::atoi(argv[2]) : 6379;
  int nreq = argc > 3 ? std::atoi(argv[3]) : 100000;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { std::perror("socket"); return 1; }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    std::fprintf(stderr, "bad host: %s\n", host);
    return 1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    std::perror("connect");
    return 1;
  }

  const std::string value(64, 'x');
  const size_t set_reply = 5;                    // "+OK\r\n" 共 5 字节
  const size_t get_reply = 5 + value.size() + 2; // "$64\r\n" + 64 字节数据 + "\r\n"

  // 预热：填充所有键。
  std::string warm;
  for (int i = 0; i < nreq; ++i) warm += SetCmd(i, value);
  if (!SendAll(fd, warm.data(), warm.size()) || !RecvAll(fd, nreq * set_reply)) {
    std::fprintf(stderr, "warm-up failed\n");
    return 1;
  }

  // SET 基准测试。
  std::string set_payload;
  for (int i = 0; i < nreq; ++i) set_payload += SetCmd(i, value);
  auto t0 = std::chrono::steady_clock::now();
  if (!SendAll(fd, set_payload.data(), set_payload.size()) || !RecvAll(fd, nreq * set_reply)) {
    std::fprintf(stderr, "SET benchmark failed\n");
    return 1;
  }
  auto t1 = std::chrono::steady_clock::now();
  double set_sec = std::chrono::duration<double>(t1 - t0).count();

  // GET 基准测试。
  std::string get_payload;
  for (int i = 0; i < nreq; ++i) get_payload += GetCmd(i);
  t0 = std::chrono::steady_clock::now();
  if (!SendAll(fd, get_payload.data(), get_payload.size()) || !RecvAll(fd, nreq * get_reply)) {
    std::fprintf(stderr, "GET benchmark failed\n");
    return 1;
  }
  t1 = std::chrono::steady_clock::now();
  double get_sec = std::chrono::duration<double>(t1 - t0).count();

  std::printf("requests (one pipelined batch): %d\n", nreq);
  std::printf("SET: %8.0f req/s  (%.3f s)\n", nreq / set_sec, set_sec);
  std::printf("GET: %8.0f req/s  (%.3f s)\n", nreq / get_sec, get_sec);

  ::close(fd);
  return 0;
}
