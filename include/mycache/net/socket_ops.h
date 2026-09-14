#pragma once

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace mycache {
namespace sockets {

inline void SetReuseAddr(int fd, bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
}

inline void SetTcpNoDelay(int fd, bool on) {
  int opt = on ? 1 : 0;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof opt);
}

inline void Close(int fd) { ::close(fd); }

inline std::string PeerToString(const sockaddr_in& addr) {
  char buf[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
  return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

// 创建一个非阻塞、带 SO_REUSEADDR 的监听套接字，绑定到 bind_ip:port。
// 返回 fd；失败返回 -1 并在 *err 中写入错误信息。
inline int CreateListenSocket(const std::string& bind_ip, int port, int backlog,
                              std::string* err) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = std::string("socket() failed: ") + strerror(errno);
    return -1;
  }
  SetReuseAddr(fd, true);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind_ip.empty() || bind_ip == "*" || bind_ip == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
    *err = "invalid bind address: " + bind_ip;
    ::close(fd);
    return -1;
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    *err = std::string("bind() failed: ") + strerror(errno);
    ::close(fd);
    return -1;
  }
  if (::listen(fd, backlog) < 0) {
    *err = std::string("listen() failed: ") + strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace sockets
}  // namespace mycache
