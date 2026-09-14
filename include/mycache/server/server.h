#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "mycache/command/command_table.h"
#include "mycache/common/config.h"
#include "mycache/net/acceptor.h"
#include "mycache/net/connection.h"
#include "mycache/net/event_loop.h"
#include "mycache/net/reactor.h"
#include "mycache/protocol/resp_value.h"
#include "mycache/server/command_router.h"

namespace mycache {

class AofWriter;

// 持有主反应器（负责 accept）、一组 IO 反应器（工作线程）、命令路由器
// （一组 shard worker）以及命令表，将网络层与命令分发连接起来。
class Server {
 public:
  explicit Server(const Config& config);
  ~Server();

  void Start();
  void Stop();

 private:
  void OnNewConnection(int fd, const std::string& peer);
  void OnMessage(const std::shared_ptr<Connection>& conn, Buffer* buf);
  void DispatchCommand(const std::shared_ptr<Connection>& conn, const RespValue& cmd);
  void HandleSignalRead();
  bool InitAof();  // 启动前恢复 + 打开 AOF；失败返回 false

  Config config_;
  CommandTable command_table_;
  CommandRouter router_;

  EventLoop main_loop_;
  std::unique_ptr<Acceptor> acceptor_;
  std::vector<std::unique_ptr<IOReactor>> io_reactors_;
  size_t next_reactor_ = 0;

  std::unique_ptr<AofWriter> aof_;

  int signal_fd_ = -1;
  std::unique_ptr<Channel> signal_channel_;
  std::atomic<bool> stopping_{false};
};

}  // namespace mycache
