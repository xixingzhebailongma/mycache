// keyed 路径集成测试：起真实 server + 真实 socket 客户端，验证 Route 的
// `args = cmd.array` 拷贝构造器物化 view → str。这是 Route 拷贝物化的唯一执行者——
// 若有人把 Route 改成 std::move 或引用捕获，view 逃出 IO 线程悬空，SET 存进空串，
// GET 返回空，本测试必挂。注释是提醒，测试才是守卫。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

const char* kServerPath = MYCACHE_SERVER_PATH;
int g_port = 0;
pid_t g_server_pid = -1;

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

// 读精确 n 字节进 out；失败/超时返回 false（5 秒 SO_RCVTIMEO，避免 view 逃逸时挂死）。
bool RecvExact(int fd, size_t n, std::string* out) {
  std::vector<char> buf(n);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, buf.data() + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    got += static_cast<size_t>(r);
  }
  out->assign(buf.data(), n);
  return true;
}

int Connect(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  timeval tv{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int GetFreePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

std::string SetCmd(const std::string& k, const std::string& v) {
  return "*3\r\n$3\r\nSET\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n$" +
         std::to_string(v.size()) + "\r\n" + v + "\r\n";
}

std::string GetCmd(const std::string& k) {
  return "*2\r\n$3\r\nGET\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
}

}  // namespace

class KeyedViewIntegrationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    g_port = GetFreePort();
    char conf[256];
    std::snprintf(conf, sizeof conf, "/tmp/mycache_keyed_test_%d.conf", g_port);
    FILE* f = std::fopen(conf, "w");
    std::fprintf(f,
                 "port %d\nbind 127.0.0.1\nio_threads 2\nshard_count 4\n"
                 "tcp_backlog 511\nlog_level error\n",
                 g_port);
    std::fclose(f);

    g_server_pid = ::fork();
    if (g_server_pid == 0) {
      ::execl(kServerPath, kServerPath, conf, nullptr);
      ::_exit(127);
    }
    // 等 ready：反复 connect + PING，直到 PONG。
    for (int i = 0; i < 200; ++i) {
      int fd = Connect(g_port);
      if (fd >= 0) {
        std::string reply;
        if (SendAll(fd, "*1\r\n$4\r\nPING\r\n", 14) && RecvExact(fd, 7, &reply) &&
            reply == "+PONG\r\n") {
          ::close(fd);
          break;
        }
        ::close(fd);
      }
      ::usleep(10 * 1000);
    }
  }

  static void TearDownTestSuite() {
    if (g_server_pid > 0) {
      ::kill(g_server_pid, SIGINT);
      int status = 0;
      ::waitpid(g_server_pid, &status, 0);
    }
  }
};

TEST_F(KeyedViewIntegrationTest, SetGetRoundTripThroughRoute) {
  int fd = Connect(g_port);
  ASSERT_GE(fd, 0);

  const std::string key = "routekey";
  const std::string value = "routevalue";  // 10 字节

  // SET → "+OK\r\n"（5 字节）
  std::string cmd = SetCmd(key, value);
  ASSERT_TRUE(SendAll(fd, cmd.data(), cmd.size()));
  std::string reply;
  ASSERT_TRUE(RecvExact(fd, 5, &reply));
  EXPECT_EQ(reply, "+OK\r\n");

  // GET → "$10\r\nroutevalue\r\n"。若 Route 改 move/引用捕获 → 空串，这里挂。
  cmd = GetCmd(key);
  ASSERT_TRUE(SendAll(fd, cmd.data(), cmd.size()));
  const std::string expect = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
  ASSERT_TRUE(RecvExact(fd, expect.size(), &reply));
  EXPECT_EQ(reply, expect);

  ::close(fd);
}
