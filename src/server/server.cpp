#include "mycache/server/server.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
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

namespace mycache {

namespace {

Server* g_server = nullptr;
int g_signal_fd = -1;

void SignalHandler(int /*sig*/) {
  uint64_t one = 1;
  if (g_signal_fd >= 0) {
    ssize_t n = ::write(g_signal_fd, &one, sizeof one);
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

  // 启动前恢复 AOF（在 shard worker 线程启动之前，直接写入各 shard，单线程安全）。
  if (!InitAof()) {
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
  if (aof_) aof_->Close();  // 优雅关闭：flush + fsync
  main_loop_.Quit();
}

bool Server::InitAof() {
  if (!config_.appendonly) return true;

  AofFsync fsync = AofFsync::kEverysec;
  if (!ParseAofFsync(config_.appendfsync, &fsync)) {
    LOG_ERROR("invalid appendfsync value: %s", config_.appendfsync.c_str());
    return false;
  }

  const std::string path = config_.AofPath();

  // 1) 恢复：逐条重放并修复尾部不完整记录。shard worker 尚未启动，可安全直接写入。
  auto result = AofRecovery::Replay(path, [this](const std::string& name,
                                                 const std::vector<std::string>& args) {
    if (args.empty()) return false;
    size_t idx = router_.ShardIndex(args[0]);
    return ApplyAofCommand(router_.ShardAt(idx), name, args);
  });
  if (!result.ok) {
    LOG_ERROR("AOF recovery failed: %s", result.error.c_str());
    return false;
  }
  LOG_INFO("AOF recovery: replayed %llu command(s), truncated %llu trailing byte(s)",
           static_cast<unsigned long long>(result.commands_replayed),
           static_cast<unsigned long long>(result.bytes_truncated));

  // 2) 打开追加写入。
  aof_ = std::make_unique<AofWriter>();
  if (!aof_->Open(path, fsync)) {
    aof_.reset();
    return false;
  }
  return true;
}

void Server::HandleSignalRead() {
  uint64_t v = 0;
  ssize_t n = ::read(signal_fd_, &v, sizeof v);
  (void)n;
  LOG_INFO("signal received, shutting down");
  Stop();
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
  RespParser parser;
  while (buf->readable() > 0) {
    // 跳过多余的 CR/LF 字节（例如 redis-cli --pipe 在其 ECHO 哨兵之前追加的
    // 结尾 CRLF）。
    while (buf->readable() > 0 && (buf->peek()[0] == '\r' || buf->peek()[0] == '\n')) {
      buf->retrieve(1);
    }
    if (buf->readable() == 0) break;

    size_t consumed = 0;
    RespValue cmd;
    ParseResult r = parser.Parse(buf->peek(), buf->readable(), &consumed, &cmd);
    if (r == ParseResult::kIncomplete) return;  // 等待更多字节
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

  if (command->first_key > 0) {
    // 有 key 的命令：按 key 哈希路由到对应 shard worker，在其线程上执行。
    const std::string& key = cmd.array[static_cast<size_t>(command->first_key)].str;
    AofWriter* aof = aof_.get();
    const Config* cfg = &config_;
    CommandRouter* rt = &router_;
    router_.Route(key, [conn, handler = command->handler, args = cmd.array, aof, cfg, rt](Shard* shard) {
      CommandContext ctx{conn, shard};
      ctx.aof = aof;
      ctx.config = cfg;
      ctx.router = rt;
      handler(ctx, args);
    });
  } else {
    // 无 key 的命令（PING/ECHO/INFO）：在 IO 线程直接执行。
    CommandContext ctx{conn, nullptr};
    ctx.aof = aof_.get();
    ctx.config = &config_;
    ctx.router = &router_;
    command->handler(ctx, cmd.array);
  }
}

}  // namespace mycache
