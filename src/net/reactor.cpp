#include "mycache/net/reactor.h"

#include "mycache/monitor/metrics.h"

namespace mycache {

IOReactor::~IOReactor() { Stop(); }

void IOReactor::Start() {
  thread_ = std::thread([this] { loop_.Loop(); });
}

void IOReactor::Stop() {
  if (thread_.joinable()) {
    loop_.Quit();
    thread_.join();
  }
}

void IOReactor::AddConnection(int fd, const std::string& peer,
                              Connection::MessageCallback msg_cb) {
  loop_.RunInLoop([this, fd, peer, cb = std::move(msg_cb)]() mutable {
    auto conn = std::make_shared<Connection>(&loop_, fd, peer);
    conn->SetMessageCallback(std::move(cb));
    conn->SetCloseCallback([this](const std::shared_ptr<Connection>& c) {
      connections_.erase(c->fd());
      connection_count_.fetch_sub(1);
      Metrics::Instance().DecrConnected();
    });
    connections_[fd] = conn;
    connection_count_.fetch_add(1);
    Metrics::Instance().IncrConnected();
    conn->Establish();
  });
}

}  // namespace mycache
