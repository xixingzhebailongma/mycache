#include "mycache/server/server.h"

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstring>
#include <string>

#include "mycache/command/command.h"
#include "mycache/command/command_context.h"
#include "mycache/common/logger.h"
#include "mycache/eviction/eviction_policy.h"
#include "mycache/eviction/lru.h"
#include "mycache/monitor/metrics.h"
#include "mycache/net/socket_ops.h"
#include "mycache/persistence/aof/aof_recovery.h"
#include "mycache/persistence/aof/aof_writer.h"
#include "mycache/protocol/resp_encoder.h"
#include "mycache/protocol/resp_parser.h"
#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"
#include "mycache/storage/object/string_object.h"

namespace mycache {

namespace {

Server* g_server = nullptr;
int g_signal_fd = -1;
int g_child_fd = -1;

// 自动 AOF rewrite 的检查周期（秒）。timerfd 每隔这么久到期一次，主循环读空到期计数
// 后调 PersistenceManager::MaybeAutoRewrite()。
constexpr int kAutoRewriteIntervalSec = 1;

void SignalHandler(int /*sig*/) {
  uint64_t one = 1;
  if (g_signal_fd >= 0) {
    ssize_t n = ::write(g_signal_fd, &one, sizeof one);
    (void)n;
  }
}

// SIGCHLD 处理器：只做 async-signal-safe 的 eventfd 写入，真正的 waitpid 收割
// 放到主循环的 HandleChildExit 里。
void ChildSignalHandler(int /*sig*/) {
  uint64_t one = 1;
  if (g_child_fd >= 0) {
    ssize_t n = ::write(g_child_fd, &one, sizeof one);
    (void)n;
  }
}

// 把 Config 里的内存治理字段转成 EvictionConfig；策略名非法时告警并回退 noeviction。
EvictionConfig MakeEvictionConfig(const Config& c) {
  EvictionConfig ec;
  ec.maxmemory = c.maxmemory;
  ec.maxmemory_samples = c.maxmemory_samples;
  EvictionPolicy p;
  if (!ParseEvictionPolicy(c.maxmemory_policy, &p)) {
    LOG_WARN("invalid maxmemory_policy '%s', falling back to noeviction", c.maxmemory_policy.c_str());
  } else {
    ec.policy = p;
  }
  return ec;
}

}  // namespace

Server::Server(const Config& config)
    : config_(config), router_(config.shard_count, MakeEvictionConfig(config)) {
  RegisterStringCommands(&command_table_);
  RegisterGenericCommands(&command_table_);
  RegisterHashCommands(&command_table_);
  RegisterListCommands(&command_table_);
  RegisterServerCommands(&command_table_);
}

Server::~Server() {
  Stop();
  if (signal_fd_ >= 0) sockets::Close(signal_fd_);
}

void Server::Start() {
  if (stopping_) return;
  ::signal(SIGPIPE, SIG_IGN);
  Metrics::Instance().MarkStart();
  RefreshLruClock();
  CreateSharedObjects();  // 共享整数池（0..9999），供 TryObjectEncoding 复用

  // 启动前恢复 AOF（在 shard worker 线程启动之前，直接写入各 shard，单线程安全）。
  if (!pm_.Init(config_, &router_)) {
    LOG_FATAL("AOF initialization failed, refusing to start");
    return;
  }

  // 启动 IO 反应器线程。
  io_reactors_.reserve(static_cast<size_t>(config_.io_threads));
  for (int i = 0; i < config_.io_threads; ++i) {
    io_reactors_.push_back(std::make_unique<IOReactor>());
    io_reactors_.back()->Start();
  }

  // 启动 shard worker 线程（每个 shard 一个）。
  router_.Start();

  // 信号驱动的关闭流程：信号处理器向 eventfd 写入，主循环读回该 eventfd，
  // 从而使 Stop() 在循环线程上执行。
  g_server = this;
  signal_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  g_signal_fd = signal_fd_;
  signal_channel_ = std::make_unique<Channel>(&main_loop_, signal_fd_);
  signal_channel_->SetReadCallback([this] { HandleSignalRead(); });
  signal_channel_->EnableReading();
  ::signal(SIGINT, SignalHandler);
  ::signal(SIGTERM, SignalHandler);

  child_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  g_child_fd = child_fd_;
  child_channel_ = std::make_unique<Channel>(&main_loop_, child_fd_);
  child_channel_->SetReadCallback([this] { HandleChildExit(); });
  child_channel_->EnableReading();
  ::signal(SIGCHLD, ChildSignalHandler);

  // 自动 AOF rewrite 检查定时器：create/settime 任一步失败都记日志并禁用（保持
  // tick_fd_ = -1，HandleAutoRewriteTick 对 -1 直接返回），不因此拒绝启动。
  tick_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tick_fd_ < 0) {
    LOG_WARN("timerfd_create failed, auto AOF rewrite disabled: %s", std::strerror(errno));
  } else {
    struct itimerspec its {};
    its.it_interval.tv_sec = kAutoRewriteIntervalSec;
    its.it_value.tv_sec = kAutoRewriteIntervalSec;
    if (::timerfd_settime(tick_fd_, 0, &its, nullptr) != 0) {
      LOG_WARN("timerfd_settime failed, auto AOF rewrite disabled: %s", std::strerror(errno));
      sockets::Close(tick_fd_);
      tick_fd_ = -1;
    } else {
      tick_channel_ = std::make_unique<Channel>(&main_loop_, tick_fd_);
      tick_channel_->SetReadCallback([this] { HandleAutoRewriteTick(); });
      tick_channel_->EnableReading();
    }
  }

  acceptor_ = std::make_unique<Acceptor>(&main_loop_, config_.bind, config_.port, config_.tcp_backlog);
  if (!acceptor_->valid()) {
    LOG_FATAL("failed to create acceptor on %s:%d", config_.bind.c_str(), config_.port);
    return;
  }
  acceptor_->SetNewConnectionCallback([this](int fd, const std::string& peer) {
    OnNewConnection(fd, peer);
  });

  LOG_INFO("mycache-server listening on %s:%d with %d io thread(s)",
           config_.bind.c_str(), config_.port, config_.io_threads);

  main_loop_.Loop();

  Stop();
}

void Server::Stop() {
  if (stopping_.exchange(true)) return;
  acceptor_.reset();
  for (auto& r : io_reactors_) r->Stop();
  router_.Stop();
  pm_.Close();  // 优雅关闭：flush + fsync
  // 先摘除 tick Channel（从 epoll + channels_ 移除）再关 fd，避免 EventLoop 里残留
  // 指向已关 fd 的 Channel（fd 号可能被复用）。Channel 无析构，reset() 前必须显式 Remove()。
  if (tick_channel_) {
    tick_channel_->Remove();
    tick_channel_.reset();
  }
  if (tick_fd_ >= 0) sockets::Close(tick_fd_);
  tick_fd_ = -1;
  main_loop_.Quit();
}

void Server::HandleSignalRead() {
  uint64_t v = 0;
  ssize_t n = ::read(signal_fd_, &v, sizeof v);
  (void)n;
  LOG_INFO("signal received, shutting down");
  Stop();
}

void Server::HandleChildExit() {
  uint64_t v = 0;
  ssize_t n = ::read(child_fd_, &v, sizeof v);
  (void)n;
  // 一个 SIGCHLD 可能对应多个已退出子进程（信号会合并），循环收割。
  int status = 0;
  pid_t pid;
  while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
    pm_.OnChildExit(pid, status);
  }
}

void Server::HandleAutoRewriteTick() {
  if (tick_fd_ < 0) return;  // timerfd 创建/settime 失败已禁用
  uint64_t expirations = 0;
  ssize_t n = ::read(tick_fd_, &expirations, sizeof expirations);
  // 只有读满 8 字节（一次到期计数）才触发检查；EAGAIN（已被消费）/EINTR/短读都跳过。
  if (n != static_cast<ssize_t>(sizeof expirations)) return;
  pm_.MaybeAutoRewrite();
}

void Server::OnNewConnection(int fd, const std::string& peer) {
  if (stopping_) {
    sockets::Close(fd);
    return;
  }
  Metrics::Instance().IncrConnections();
  IOReactor* reactor = io_reactors_[next_reactor_ % io_reactors_.size()].get();
  ++next_reactor_;
  reactor->AddConnection(fd, peer, [this](const std::shared_ptr<Connection>& conn, Buffer* buf) {
    OnMessage(conn, buf);
  });
}

void Server::OnMessage(const std::shared_ptr<Connection>& conn, Buffer* buf) {
  RespParser& parser = conn->parser();
  while (buf->readable() > 0) {
    size_t consumed = 0;
    RespValue cmd;
    ParseResult r = parser.Parse(buf->peek(), buf->readable(), &consumed, &cmd);
    if (r == ParseResult::kIncomplete) {
      // 有状态增量解析：半包时 consumed 也可能 > 0，必须丢弃已消费前缀，否则下次
      // 会重复解析同一段。
      buf->retrieve(consumed);
      return;
    }
    if (r == ParseResult::kError) {
      conn->Send(EncodeResp(RespValue::Error("ERR Protocol error: invalid multibulk length")));
      conn->Shutdown();
      return;
    }
    buf->retrieve(consumed);
    DispatchCommand(conn, cmd);
  }
}

void Server::DispatchCommand(const std::shared_ptr<Connection>& conn, const RespValue& cmd) {
  if (cmd.type != RespType::kArray || cmd.array.empty()) {
    conn->Send(EncodeResp(RespValue::Error("ERR Protocol error: expected array of bulk strings")));
    return;
  }
  const RespValue& name_val = cmd.array[0];
  if (name_val.type != RespType::kBulkString) {
    conn->Send(EncodeResp(RespValue::Error("ERR Protocol error: command name must be a bulk string")));
    return;
  }

  std::string name = name_val.str;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  const Command* command = command_table_.Find(name);
  if (!command) {
    conn->Send(EncodeResp(RespValue::Error("ERR unknown command '" + name + "'")));
    return;
  }

  std::string arity_err;
  if (!CommandTable::CheckArity(*command, cmd.array.size(), &arity_err)) {
    conn->Send(EncodeResp(RespValue::Error(arity_err)));
    return;
  }

  Metrics::Instance().IncrCommands();

  if (command->last_key != 0) {
    // 多 key 命令（DEL/EXISTS/MGET/MSET）：在 IO 线程执行，handler 内部按 shard
    // 分组扇出，异步汇总后回包。ctx.db 置空（handler 用 ctx.router 触达各 shard）。
    CommandContext ctx{conn, nullptr};
    ctx.aof = pm_.aof();
    ctx.config = &config_;
    ctx.router = &router_;
    ctx.pm = &pm_;
    command->handler(ctx, cmd.array);
  } else if (command->first_key > 0) {
    // 有 key 的命令：按 key 哈希路由到对应 shard worker，在其线程上执行。
    const std::string& key = cmd.array[static_cast<size_t>(command->first_key)].str;
    AofWriter* aof = pm_.aof();
    const Config* cfg = &config_;
    CommandRouter* rt = &router_;
    PersistenceManager* pm = &pm_;
    router_.Route(key, [conn, handler = command->handler, args = cmd.array, aof, cfg, rt, pm](Shard* shard) {
      CommandContext ctx{conn, shard};
      ctx.aof = aof;
      ctx.config = cfg;
      ctx.router = rt;
      ctx.pm = pm;
      handler(ctx, args);
    });
  } else {
    // 无 key 的命令（PING/ECHO/INFO）：在 IO 线程直接执行。
    CommandContext ctx{conn, nullptr};
    ctx.aof = pm_.aof();
    ctx.config = &config_;
    ctx.router = &router_;
    ctx.pm = &pm_;
    command->handler(ctx, cmd.array);
  }
}

}  // namespace mycache
