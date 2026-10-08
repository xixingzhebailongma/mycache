// kill -9 崩溃恢复集成测试：起真实 server 子进程 + 真实 socket 客户端，验证
// 「在 appendfsync always 下，任何收到成功响应的写入，kill -9 重启后必在」。
//
// 只证明【进程崩溃一致性】（kill -9 杀进程、页缓存存活），不证明【掉电持久性】：
// everysec 在掉电下最多丢约 1 秒，always 的 ack 才等价于已 fsync。
//
// 方法：客户端维护一份「仅 ack 后更新」的 Model（期望状态），每个 crash 周期
//   spawn server -> 随机写(更新 model) -> kill -9 -> 重启(重放 AOF) -> 逐 key 校验
//   -> 写 probe 再 kill 再校验(恢复后可写可再 fsync)。
//
// 校验必须 round-trip（发一条读一条），不能管道化——服务端 shard worker 异步写回，
// 同一连接管道化命令的回复会乱序（见 src/net/connection.cpp Send/RunInLoop）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mycache/persistence/aof/aof_format.h"  // aof::EncodeCommand（复用现成命令编码）

namespace {

const char* kServerPath = MYCACHE_SERVER_PATH;

// 规模参数：每场景 crash 次数 / 每次随机写条数 / keyspace 大小。
constexpr int kCycles = 20;
constexpr int kOpsPerCycle = 60;
constexpr int kStrings = 64, kHashes = 2, kHashFields = 8, kLists = 2;
constexpr int kRewriteBulkKeys = 8;
constexpr int kRewriteBulkBytes = 8192;
constexpr int kConcurrentThreads = 4;
// fd 继承测试：写足量数据拉长子进程写盘窗口，方便在 child 退出前 SIGSTOP 抓到。
constexpr int kFdCheckBulkKeys = 16;
constexpr int kFdCheckBulkBytes = 64 * 1024;  // 16×64KB = 1MB

// ---------------------------------------------------------------- socket 基础

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

bool RecvLine(int fd, std::string* line) {
  line->clear();
  char c;
  while (true) {
    ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0) return false;
    if (c == '\n') {
      if (!line->empty() && line->back() == '\r') line->pop_back();
      return true;
    }
    line->push_back(c);
  }
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
  (void)::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  socklen_t len = sizeof addr;
  (void)::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

// ---------------------------------------------------------------- RESP 回复解析（客户端侧）

struct Reply {
  enum class Kind { Simple, Error, Int, Bulk, Array } kind = Kind::Simple;
  std::string str;            // Simple/Error/Bulk 载荷
  int64_t integer = 0;        // Int
  std::vector<Reply> array;   // Array 元素
  bool is_null_bulk = false;  // $-1

  static Reply Err(const std::string& msg) {
    Reply r;
    r.kind = Kind::Error;
    r.str = msg;
    return r;
  }
};

bool ReadReply(int fd, Reply* r) {
  std::string line;
  if (!RecvLine(fd, &line) || line.empty()) return false;
  char t = line[0];
  std::string rest = line.substr(1);
  switch (t) {
    case '+': r->kind = Reply::Kind::Simple; r->str = rest; return true;
    case '-': r->kind = Reply::Kind::Error;  r->str = rest; return true;
    case ':': r->kind = Reply::Kind::Int; r->integer = std::stoll(rest); return true;
    case '$': {
      int n = std::stoi(rest);
      r->kind = Reply::Kind::Bulk;
      if (n == -1) { r->is_null_bulk = true; return true; }
      if (!RecvExact(fd, n, &r->str)) return false;
      std::string crlf;
      return RecvExact(fd, 2, &crlf);
    }
    case '*': {
      int n = std::stoi(rest);
      r->kind = Reply::Kind::Array;
      for (int i = 0; i < n; ++i) {
        Reply e;
        if (!ReadReply(fd, &e)) return false;
        r->array.push_back(std::move(e));
      }
      return true;
    }
    default: return false;
  }
}

// round-trip 客户端：发一条命令、读一条回复。
class Client {
 public:
  explicit Client(int port) : fd_(Connect(port)) {}
  ~Client() { if (fd_ >= 0) ::close(fd_); }
  bool ok() const { return fd_ >= 0; }

  // 只发送、不读回复（unacked_tail 场景的 in-flight 写用）。
  bool Send(const std::string& name, const std::vector<std::string>& args) {
    if (fd_ < 0) return false;
    std::string cmd = mycache::aof::EncodeCommand(name, args);
    return SendAll(fd_, cmd.data(), cmd.size());
  }

  Reply Do(const std::string& name, const std::vector<std::string>& args) {
    if (!Send(name, args)) return Reply::Err("send failed");
    Reply r;
    if (!ReadReply(fd_, &r)) return Reply::Err("read failed");
    return r;
  }

 private:
  int fd_;
};

// 管道化客户端：多条命令攒成一次 send，再按序读回所有回复。
class PipelineClient {
 public:
  explicit PipelineClient(int port) : fd_(Connect(port)) {}
  ~PipelineClient() { if (fd_ >= 0) ::close(fd_); }
  bool ok() const { return fd_ >= 0; }
  void Add(const std::string& name, const std::vector<std::string>& args) {
    buf_ += mycache::aof::EncodeCommand(name, args);
  }
  bool Flush() { return SendAll(fd_, buf_.data(), buf_.size()); }
  bool ReadReplies(size_t n, std::vector<Reply>* out) {
    out->clear();
    for (size_t i = 0; i < n; ++i) {
      Reply r;
      if (!ReadReply(fd_, &r)) return false;
      out->push_back(std::move(r));
    }
    return true;
  }
 private:
  int fd_;
  std::string buf_;
};

// ---------------------------------------------------------------- server 生命周期

std::string WriteConf(const std::string& dir, int port, const std::string& fsync) {
  std::string path = dir + "/mycache.conf";
  FILE* f = std::fopen(path.c_str(), "w");
  std::fprintf(f,
               "port %d\nbind 127.0.0.1\nio_threads 2\nshard_count 4\n"
               "log_level info\nio_engine posix\n"
               "appendonly yes\nappendfsync %s\ndir %s\nappendfilename appendonly.aof\n",
               port, fsync.c_str(), dir.c_str());
  std::fclose(f);
  return path;
}

// fork 出 server；子进程 setsid 成独立进程组，好让 killpg 连同 rewrite 子进程一起杀。
// stdout/stderr 重定向到 /dev/null，避免 server 的 info 日志污染测试输出。
pid_t SpawnServer(const std::string& conf) {
  pid_t pid = ::fork();
  if (pid == 0) {
    (void)::setsid();  // 独立会话/进程组（失败也可继续，仅影响 killpg 范围）
    int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::execl(kServerPath, kServerPath, conf.c_str(), nullptr);
    ::_exit(127);
  }
  return pid;
}

// 单次 connect + PING 探测，不循环。
bool PingOnce(int port) {
  int fd = Connect(port);
  if (fd < 0) return false;
  std::string reply;
  bool ok = SendAll(fd, "*1\r\n$4\r\nPING\r\n", 14) && RecvExact(fd, 7, &reply) &&
            reply == "+PONG\r\n";
  ::close(fd);
  return ok;
}

bool WaitReady(int port) {
  for (int i = 0; i < 500; ++i) {
    if (PingOnce(port)) return true;
    ::usleep(10 * 1000);
  }
  return false;
}

// kill -9 整个进程组（server + 可能的 rewrite 子进程），并收割父进程。
void KillNine(pid_t pid) {
  if (pid <= 0) return;
  ::killpg(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

// 重启并等 ready。rewrite 子进程被 SIGKILL 后 fd 释放是异步的（waitpid 只等父进程），
// 短暂占着端口；bind 冲突（server 秒退）时等一下重试。返回 pid，失败返回 -1。
pid_t RestartReady(const std::string& conf, int port) {
  pid_t last = -1;
  for (int attempt = 0; attempt < 20; ++attempt) {
    last = SpawnServer(conf);
    for (int i = 0; i < 200; ++i) {
      if (PingOnce(port)) return last;
      int status = 0;
      if (::waitpid(last, &status, WNOHANG) == last) break;  // 已退出（bind 失败）
      ::usleep(10 * 1000);
    }
    ::usleep(50 * 1000);  // 等端口释放再试
  }
  if (last > 0) KillNine(last);
  return -1;
}

// ---------------------------------------------------------------- 模型（客户端期望状态）

struct Keyspace {
  std::vector<std::string> strings;
  std::vector<std::pair<std::string, std::vector<std::string>>> hashes;  // hkey -> fields
  std::vector<std::string> lists;
};

struct Model {
  std::unordered_map<std::string, std::string> s;  // string
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> h;  // hash
  std::unordered_map<std::string, std::deque<std::string>> l;  // list
  std::unordered_set<std::string> ttl;  // 有 TTL 的 key
};

Keyspace MakeKeyspace() {
  Keyspace ks;
  for (int i = 0; i < kStrings; ++i) ks.strings.push_back("k" + std::to_string(i));
  for (int i = 0; i < kHashes; ++i) {
    std::vector<std::string> fields;
    for (int j = 0; j < kHashFields; ++j) fields.push_back("f" + std::to_string(j));
    ks.hashes.push_back({"h" + std::to_string(i), std::move(fields)});
  }
  for (int i = 0; i < kLists; ++i) ks.lists.push_back("l" + std::to_string(i));
  return ks;
}

void SeedModel(Client& c, Model& m, const Keyspace& ks) {
  c.Do("SET", {ks.strings[0], "seed0"});
  m.s[ks.strings[0]] = "seed0";
  c.Do("EXPIRE", {ks.strings[0], "100000"});
  m.ttl.insert(ks.strings[0]);
  c.Do("HSET", {ks.hashes[0].first, ks.hashes[0].second[0], "seedh"});
  m.h[ks.hashes[0].first][ks.hashes[0].second[0]] = "seedh";
  c.Do("RPUSH", {ks.lists[0], "seeda"});
  m.l[ks.lists[0]].push_back("seeda");
  c.Do("RPUSH", {ks.lists[0], "seedb"});
  m.l[ks.lists[0]].push_back("seedb");
}

// ---------------------------------------------------------------- 随机写（ack-gated）

struct OpWeight {
  std::string kind;
  double w;
};

const std::vector<OpWeight> kStandardWeights = {
    {"SET", 0.40}, {"DEL", 0.15}, {"HSET", 0.15}, {"HDEL", 0.08}, {"RPUSH", 0.08},
    {"LPUSH", 0.04}, {"LPOP", 0.05}, {"RPOP", 0.03}, {"EXPIRE", 0.02}};

const std::vector<OpWeight> kListWeights = {
    {"RPUSH", 0.30}, {"LPUSH", 0.20}, {"LPOP", 0.25}, {"RPOP", 0.25}};

const std::vector<OpWeight> kStringWeights = {{"SET", 0.6}, {"DEL", 0.4}};

std::string PickKind(std::mt19937& rng, std::uniform_real_distribution<double>& r01,
                     const std::vector<OpWeight>& weights) {
  double wsum = 0;
  for (auto& w : weights) wsum += w.w;
  double r = r01(rng) * wsum;
  double acc = 0;
  for (auto& w : weights) {
    acc += w.w;
    if (r < acc) return w.kind;
  }
  return weights.back().kind;
}

// 随机写；只在收到成功响应后更新模型。返回新 seq。
int DoRandomOps(Client& c, Model& m, const Keyspace& ks, std::mt19937& rng, int n, int seq,
                const std::vector<OpWeight>& weights) {
  std::uniform_real_distribution<double> r01(0.0, 1.0);
  auto pick_str = [&] { return ks.strings[rng() % ks.strings.size()]; };
  auto pick_hash = [&]() -> std::pair<std::string, std::string> {
    auto& h = ks.hashes[rng() % ks.hashes.size()];
    return {h.first, h.second[rng() % h.second.size()]};
  };
  auto pick_list = [&] { return ks.lists[rng() % ks.lists.size()]; };

  for (int i = 0; i < n; ++i) {
    std::string kind = PickKind(rng, r01, weights);
    if (kind == "SET") {
      std::string k = pick_str(), v = "v" + std::to_string(seq++);
      Reply r = c.Do("SET", {k, v});
      if (r.kind == Reply::Kind::Simple) {
        m.s[k] = v;
        m.ttl.erase(k);  // SET 清除 TTL（src/storage/db/shard.cpp Set）
      }
    } else if (kind == "DEL") {
      std::string k = pick_str();
      Reply r = c.Do("DEL", {k});
      if (r.kind == Reply::Kind::Int) {
        m.s.erase(k);
        m.ttl.erase(k);
      }
    } else if (kind == "HSET") {
      auto [h, f] = pick_hash();
      std::string v = "v" + std::to_string(seq++);
      Reply r = c.Do("HSET", {h, f, v});
      if (r.kind == Reply::Kind::Int) m.h[h][f] = v;
    } else if (kind == "HDEL") {
      auto [h, f] = pick_hash();
      Reply r = c.Do("HDEL", {h, f});
      if (r.kind == Reply::Kind::Int) {
        auto it = m.h.find(h);
        if (it != m.h.end()) it->second.erase(f);
      }
    } else if (kind == "RPUSH") {
      std::string l = pick_list(), v = "v" + std::to_string(seq++);
      Reply r = c.Do("RPUSH", {l, v});
      if (r.kind == Reply::Kind::Int) m.l[l].push_back(v);
    } else if (kind == "LPUSH") {
      std::string l = pick_list(), v = "v" + std::to_string(seq++);
      Reply r = c.Do("LPUSH", {l, v});
      if (r.kind == Reply::Kind::Int) m.l[l].push_front(v);
    } else if (kind == "LPOP") {
      std::string l = pick_list();
      Reply r = c.Do("LPOP", {l});
      if (r.kind == Reply::Kind::Bulk && !r.is_null_bulk) {
        auto it = m.l.find(l);
        if (it != m.l.end()) {
          it->second.pop_front();
          if (it->second.empty()) m.l.erase(it);
        }
      }
    } else if (kind == "RPOP") {
      std::string l = pick_list();
      Reply r = c.Do("RPOP", {l});
      if (r.kind == Reply::Kind::Bulk && !r.is_null_bulk) {
        auto it = m.l.find(l);
        if (it != m.l.end()) {
          it->second.pop_back();
          if (it->second.empty()) m.l.erase(it);
        }
      }
    } else if (kind == "EXPIRE") {
      std::string k = pick_str();
      Reply r = c.Do("EXPIRE", {k, "100000"});
      if (r.kind == Reply::Kind::Int && r.integer == 1) m.ttl.insert(k);
    }
  }
  return seq;
}

// ---------------------------------------------------------------- 校验

struct Counters {
  int lost = 0, wrong = 0, resurrected = 0, load_fail = 0, probe_fail = 0;
  bool ok() const {
    return lost == 0 && wrong == 0 && resurrected == 0 && load_fail == 0 && probe_fail == 0;
  }
  std::string Describe() const {
    return "lost=" + std::to_string(lost) + " wrong=" + std::to_string(wrong) +
           " resurrected=" + std::to_string(resurrected) +
           " load_fail=" + std::to_string(load_fail) +
           " probe_fail=" + std::to_string(probe_fail);
  }
};

void Verify(Client& c, const Keyspace& ks, const Model& m, Counters* ct) {
  for (const auto& k : ks.strings) {
    Reply r = c.Do("GET", {k});
    auto it = m.s.find(k);
    bool want_present = it != m.s.end();
    bool got_present = r.kind == Reply::Kind::Bulk && !r.is_null_bulk;
    std::string got = r.kind == Reply::Kind::Bulk ? r.str : "";
    if (!want_present) {
      if (got_present) ct->resurrected++;
    } else {
      if (!got_present) ct->lost++;
      else if (got != it->second) ct->wrong++;
    }
  }
  for (const auto& [h, fields] : ks.hashes) {
    for (const auto& f : fields) {
      Reply r = c.Do("HGET", {h, f});
      auto hit = m.h.find(h);
      bool want_present = hit != m.h.end() && hit->second.count(f);
      bool got_present = r.kind == Reply::Kind::Bulk && !r.is_null_bulk;
      std::string got = r.kind == Reply::Kind::Bulk ? r.str : "";
      if (!want_present) {
        if (got_present) ct->resurrected++;
      } else {
        if (!got_present) ct->lost++;
        else if (got != hit->second.at(f)) ct->wrong++;
      }
    }
  }
  for (const auto& l : ks.lists) {
    Reply r = c.Do("LRANGE", {l, "0", "-1"});
    std::vector<std::string> got;
    if (r.kind == Reply::Kind::Array)
      for (const auto& e : r.array)
        if (e.kind == Reply::Kind::Bulk) got.push_back(e.str);
    auto lit = m.l.find(l);
    if (lit == m.l.end()) {
      if (!got.empty()) ct->resurrected++;
    } else {
      std::vector<std::string> want(lit->second.begin(), lit->second.end());
      if (got.empty()) ct->lost++;
      else if (got != want) ct->wrong++;
    }
  }
  for (const auto& k : m.ttl) {
    if (!m.s.count(k)) continue;
    Reply r = c.Do("TTL", {k});
    if (!(r.kind == Reply::Kind::Int && r.integer > 0)) ct->wrong++;
  }
}

// ---------------------------------------------------------------- crash 周期

struct CycleEnv {
  std::string dir;
  std::string conf;
  int port;
};

// 每轮一个全新临时目录（独立 AOF）+ 全新端口，避免重放成本与端口残留跨轮累积。
CycleEnv MakeCycleEnv(const std::string& fsync) {
  char tmpl[] = "/tmp/mycache_crash_XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CycleEnv e;
  e.dir = dir;
  e.port = GetFreePort();
  e.conf = WriteConf(dir, e.port, fsync);
  return e;
}

void Cleanup(const std::string& dir) { std::filesystem::remove_all(dir); }

// 顺序写 -> kill -9 -> 重启 -> verify -> probe 再 kill 再校验。
void RunStandardCycle(const std::string& fsync, std::mt19937& rng, Counters* ct,
                      const std::vector<OpWeight>& weights = kStandardWeights) {
  CycleEnv e = MakeCycleEnv(fsync);
  pid_t srv = SpawnServer(e.conf);
  if (!WaitReady(e.port)) { ct->load_fail++; KillNine(srv); Cleanup(e.dir); return; }

  Model m;
  Keyspace ks = MakeKeyspace();
  {
    Client c(e.port);
    SeedModel(c, m, ks);
    DoRandomOps(c, m, ks, rng, kOpsPerCycle, 0, weights);
  }

  KillNine(srv);

  // 重启（同 conf、同 dir、同 port）：重放 AOF。
  pid_t srv2 = RestartReady(e.conf, e.port);
  if (srv2 < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client c2(e.port);
    Verify(c2, ks, m, ct);
  }

  // 恢复后仍可写并再次 fsync：SET probe -> kill -9 -> 重启 -> GET probe。
  {
    Client pc(e.port);
    if (!pc.ok() || pc.Do("SET", {"probe_refsync", "probe_val"}).kind != Reply::Kind::Simple) {
      ct->probe_fail++;
      KillNine(srv2);
      Cleanup(e.dir);
      return;
    }
  }
  KillNine(srv2);
  pid_t srv3 = RestartReady(e.conf, e.port);
  if (srv3 < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client vc(e.port);
    Reply r = vc.Do("GET", {"probe_refsync"});
    if (!(r.kind == Reply::Kind::Bulk && r.str == "probe_val")) ct->probe_fail++;
  }
  KillNine(srv3);
  Cleanup(e.dir);
}

// BGREWRITEAOF 期间 kill -9。大 value 基线拉长子进程写盘窗口，轮询 tmp 命中即杀。
void RunRewriteCycle(const std::string& fsync, std::mt19937& rng, Counters* ct) {
  CycleEnv e = MakeCycleEnv(fsync);
  pid_t srv = SpawnServer(e.conf);
  if (!WaitReady(e.port)) { ct->load_fail++; KillNine(srv); Cleanup(e.dir); return; }

  Model m;
  Keyspace ks = MakeKeyspace();
  {
    Client c(e.port);
    SeedModel(c, m, ks);
    std::string big(kRewriteBulkBytes, 'z');
    for (int i = 0; i < kRewriteBulkKeys; ++i) {
      std::string k = "bulk" + std::to_string(i);
      c.Do("SET", {k, big});
      m.s[k] = big;
    }
    c.Do("BGREWRITEAOF", {});
    // 子进程 fork 后 O_TRUNC 建 tmp，出现即进入 rewrite 窗口。
    std::string tmp = e.dir + "/appendonly.aof.rewrite.tmp";
    for (int i = 0; i < 500; ++i) {
      if (::access(tmp.c_str(), F_OK) == 0) break;
      ::usleep(1000);
    }
    DoRandomOps(c, m, ks, rng, kOpsPerCycle, 0, kStandardWeights);
  }

  KillNine(srv);

  pid_t srv2 = RestartReady(e.conf, e.port);
  if (srv2 < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client c2(e.port);
    Verify(c2, ks, m, ct);
  }
  KillNine(srv2);
  Cleanup(e.dir);
}

// 同一 AOF ≥3 轮 crash→恢复→再写→再 crash，校验多代重放 + 恢复后可写。
void RunMultiroundCycle(std::mt19937& rng, Counters* ct, int rounds = 3) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  if (!WaitReady(e.port)) { ct->load_fail++; KillNine(srv); Cleanup(e.dir); return; }

  Model m;
  Keyspace ks = MakeKeyspace();
  {
    Client c(e.port);
    SeedModel(c, m, ks);
    DoRandomOps(c, m, ks, rng, kOpsPerCycle, 0, kStandardWeights);
  }
  KillNine(srv);

  for (int round = 0; round < rounds; ++round) {
    pid_t s = RestartReady(e.conf, e.port);
    if (s < 0) { ct->load_fail++; Cleanup(e.dir); return; }
    {
      Client c(e.port);
      Verify(c, ks, m, ct);
      DoRandomOps(c, m, ks, rng, kOpsPerCycle, 0, kStandardWeights);  // 恢复后再写
    }
    KillNine(s);
  }

  pid_t sf = RestartReady(e.conf, e.port);
  if (sf < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client c(e.port);
    Verify(c, ks, m, ct);
  }
  KillNine(sf);
  Cleanup(e.dir);
}

// 多线程并发写（组提交攒批）。线程各自独立 key 前缀 + 独立 model，join 后合并（key 不相交）。
void RunConcurrentCycle(std::mt19937& rng, Counters* ct) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  if (!WaitReady(e.port)) { ct->load_fail++; KillNine(srv); Cleanup(e.dir); return; }

  const int T = kConcurrentThreads;
  const int per = std::max(1, kStrings / T);
  std::vector<Keyspace> tks(T);
  for (int t = 0; t < T; ++t)
    for (int j = 0; j < per; ++j)
      tks[t].strings.push_back("c" + std::to_string(t) + "_k" + std::to_string(j));
  Keyspace full;
  for (auto& ks : tks) full.strings.insert(full.strings.end(), ks.strings.begin(), ks.strings.end());

  std::vector<Model> models(T);
  std::vector<std::thread> threads;
  for (int t = 0; t < T; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 trng(rng() + static_cast<unsigned>(t));
      Client c(e.port);
      if (c.ok()) DoRandomOps(c, models[t], tks[t], trng, kOpsPerCycle, 0, kStringWeights);
    });
  }
  for (auto& th : threads) th.join();

  Model m;
  for (auto& tm : models)
    for (auto& [k, v] : tm.s) m.s[k] = v;

  KillNine(srv);

  pid_t srv2 = RestartReady(e.conf, e.port);
  if (srv2 < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client c2(e.port);
    Verify(c2, full, m, ct);
  }
  KillNine(srv2);
  Cleanup(e.dir);
}

// 发一批写入不等 ack 立即 kill（未确认尾部）；只校验 ack 子集。
void RunUnackedTailCycle(std::mt19937& rng, Counters* ct) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  if (!WaitReady(e.port)) { ct->load_fail++; KillNine(srv); Cleanup(e.dir); return; }

  Model m;
  Keyspace ks = MakeKeyspace();
  {
    Client c(e.port);
    SeedModel(c, m, ks);
    DoRandomOps(c, m, ks, rng, kOpsPerCycle, 0, kStandardWeights);
    // in-flight：写独立 key（x0..x7，不在 keyspace），不读回复，立即 kill。
    for (int i = 0; i < 8; ++i)
      c.Send("SET", {"x" + std::to_string(i), "inflight" + std::to_string(i)});
  }

  KillNine(srv);

  pid_t srv2 = RestartReady(e.conf, e.port);
  if (srv2 < 0) { ct->load_fail++; Cleanup(e.dir); return; }
  {
    Client c2(e.port);
    Verify(c2, ks, m, ct);
  }
  KillNine(srv2);
  Cleanup(e.dir);
}

// RAII：测试内 ASSERT 提前失败时也能杀 server、清理临时目录。
struct CrashServerGuard {
  pid_t pid = -1;
  std::string dir;
  ~CrashServerGuard() {
    if (pid > 0) KillNine(pid);
    if (!dir.empty()) Cleanup(dir);
  }
};

// 从 INFO bulk 里取一个整型字段（如 stale_replies）。
uint64_t InfoMetric(const std::string& info, const std::string& key) {
  std::string needle = key + ":";
  size_t pos = info.find(needle);
  if (pos == std::string::npos) return 0;
  pos += needle.size();
  size_t end = info.find("\r\n", pos);
  return static_cast<uint64_t>(std::stoull(info.substr(pos, end - pos)));
}

// ---------------------------------------------------------------- fd 继承检查

// 遍历 /proc/<pid>/task/*/children 收集全部子进程 pid。
// fork 发生在 IO 线程（bgrewriteaof/bgsave 都是 no-key 命令），子进程挂在
// /proc/<pid>/task/<io_tid>/children，而不是 task/<pid>/children（主线程）。
std::vector<pid_t> CollectChildren(pid_t server_pid) {
  std::vector<pid_t> children;
  std::string task_dir = "/proc/" + std::to_string(server_pid) + "/task";
  std::error_code ec;
  std::filesystem::directory_iterator it(task_dir, ec);
  if (ec) return children;  // server 已退出，无 task 目录
  for (const auto& t : it) {
    std::ifstream f((t.path() / "children").string());
    pid_t c;
    while (f >> c) children.push_back(c);
  }
  return children;
}

// 轮询 task/*/children 取第一个子进程 pid；超时返回 -1。
pid_t WaitChild(pid_t server_pid) {
  for (int i = 0; i < 500; ++i) {
    std::vector<pid_t> cs = CollectChildren(server_pid);
    if (!cs.empty()) return cs[0];
    ::usleep(1000);
  }
  return -1;
}

// 读 /proc/<pid>/fd/* 的 readlink 目标，返回是否含 socket。
bool ChildHasSocket(pid_t child_pid) {
  std::string fd_dir = "/proc/" + std::to_string(child_pid) + "/fd";
  std::error_code ec;
  std::filesystem::directory_iterator it(fd_dir, ec);
  if (ec) return false;  // child 已退出，读不到即视为无 socket
  char link[256];
  for (const auto& e : it) {
    std::string p = e.path().string();
    ssize_t n = ::readlink(p.c_str(), link, sizeof link - 1);
    if (n < 0) continue;
    link[n] = '\0';
    if (std::strncmp(link, "socket:[", 8) == 0) return true;
  }
  return false;
}

// SIGSTOP 冻结子进程直到观察到它不再持有 socket（说明 CloseInheritedFds 已跑过）。
// 返回 true = 已冻结在「无 socket」状态；false = child 已退出或 5 次仍见 socket。
// 重试防「SIGSTOP 盖在 CloseInheritedFds 前」的误红。
bool FreezeChildPastCloseFds(pid_t child) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (::kill(child, SIGSTOP) != 0) return false;  // 已退出
    if (!ChildHasSocket(child)) return true;         // 已过 CloseInheritedFds
    ::kill(child, SIGCONT);                          // 还没过，放它再跑一步
    ::usleep(1000);
  }
  ::kill(child, SIGCONT);
  return false;
}

// 主测试：bgrewriteaof/bgsave 子进程不继承监听 socket（/proc/<child>/fd 无 socket 项）。
void RunChildNoSocketFd(const std::string& cmd) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  CrashServerGuard guard{srv, e.dir};
  ASSERT_TRUE(WaitReady(e.port)) << "server failed to start";

  {
    Client c(e.port);
    ASSERT_TRUE(c.ok());
    std::string big(kFdCheckBulkBytes, 'z');
    for (int i = 0; i < kFdCheckBulkKeys; ++i)
      ASSERT_EQ(c.Do("SET", {"fd_bulk" + std::to_string(i), big}).kind, Reply::Kind::Simple);
    Reply r = c.Do(cmd, {});
    ASSERT_TRUE(r.kind == Reply::Kind::Simple || r.kind == Reply::Kind::Error)
        << "unexpected reply to " << cmd;
  }

  pid_t child = WaitChild(srv);
  ASSERT_GT(child, 0) << "no " << cmd << " child spawned";

  ASSERT_TRUE(FreezeChildPastCloseFds(child))
      << cmd << " child still holds a socket fd (CloseInheritedFds not effective)";
  ::kill(child, SIGCONT);  // 恢复 child 正常收尾；CrashServerGuard 析构会 killpg 兜底
}

// bind + SO_REUSEADDR 试探端口是否可绑；无论成败都 close。返回是否 bind 成功。
bool TryBind(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int r = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  ::close(fd);  // bind 成功后记得 close
  return r == 0;
}

// 辅助测试：rewrite/bgsave 期间 kill -9 父进程，端口应立即可重绑。
// 用 bind 检查（不用 connect——connect 会因子进程继承的 listen socket 半开而成功/挂起）。
void RunPortRebindAfterKill(const std::string& cmd) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  CrashServerGuard guard{srv, e.dir};
  ASSERT_TRUE(WaitReady(e.port)) << "server failed to start";

  {
    Client c(e.port);
    ASSERT_TRUE(c.ok());
    std::string big(kFdCheckBulkBytes, 'z');
    for (int i = 0; i < kFdCheckBulkKeys; ++i)
      ASSERT_EQ(c.Do("SET", {"fd_bulk" + std::to_string(i), big}).kind, Reply::Kind::Simple);
    Reply r = c.Do(cmd, {});
    ASSERT_TRUE(r.kind == Reply::Kind::Simple || r.kind == Reply::Kind::Error)
        << "unexpected reply to " << cmd;
  }

  pid_t child = WaitChild(srv);
  ASSERT_GT(child, 0) << "no " << cmd << " child spawned";

  // 只杀父进程（server），让子进程继续活着（孤儿），观察它是否还占着监听端口。
  ::kill(srv, SIGKILL);
  int status = 0;
  ::waitpid(srv, &status, 0);

  // 重试：SIGSTOP 冻住孤儿 child → bind 试端口。若 EADDRINUSE（SIGSTOP 盖在
  // CloseInheritedFds 前，child 尚持监听 fd）则 SIGCONT/sleep/再 SIGSTOP 重试。
  bool bound = false;
  for (int attempt = 0; attempt < 5 && !bound; ++attempt) {
    if (::kill(child, SIGSTOP) != 0) break;  // child 已退出，本轮 inconclusive
    bound = TryBind(e.port);
    if (!bound) { ::kill(child, SIGCONT); ::usleep(1000); }
  }
  ::kill(child, SIGCONT);
  ASSERT_TRUE(bound)
      << cmd << ": port still held by orphan child (CloseInheritedFds not effective)";

  // 清理孤儿子进程（其父已死，waitpid 由 init 兜底，这里只发信号）。
  ::kill(child, SIGKILL);
}

}  // namespace

// ---------------------------------------------------------------- 测试

TEST(CrashRecovery, SeqAlwaysNoLoss) {
  Counters ct;
  std::mt19937 rng(42);
  for (int i = 0; i < kCycles; ++i) RunStandardCycle("always", rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, SeqEverysecNoLoss) {
  Counters ct;
  std::mt19937 rng(43);
  for (int i = 0; i < kCycles; ++i) RunStandardCycle("everysec", rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, UnackedTailOnlyUnackedLost) {
  Counters ct;
  std::mt19937 rng(44);
  for (int i = 0; i < kCycles; ++i) RunUnackedTailCycle(rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, RewriteMidKillNoLoss) {
  Counters ct;
  std::mt19937 rng(45);
  for (int i = 0; i < kCycles; ++i) RunRewriteCycle("always", rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, RewriteEverysecMidKillNoLoss) {
  Counters ct;
  std::mt19937 rng(46);
  for (int i = 0; i < kCycles; ++i) RunRewriteCycle("everysec", rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, MultiroundAccumulationNoLoss) {
  Counters ct;
  std::mt19937 rng(47);
  for (int i = 0; i < kCycles; ++i) RunMultiroundCycle(rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, ConcurrentGroupCommitNoLoss) {
  Counters ct;
  std::mt19937 rng(48);
  for (int i = 0; i < kCycles; ++i) RunConcurrentCycle(rng, &ct);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

TEST(CrashRecovery, ListCoverageNoLoss) {
  Counters ct;
  std::mt19937 rng(49);
  for (int i = 0; i < kCycles; ++i) RunStandardCycle("always", rng, &ct, kListWeights);
  EXPECT_TRUE(ct.ok()) << ct.Describe();
}

// Bug 1 回归：同一连接管道化命令，回复必须按命令序（不能按 worker 完成序乱序）。
void RunPipelineOrder(const std::string& fsync) {
  CycleEnv e = MakeCycleEnv(fsync);
  pid_t srv = SpawnServer(e.conf);
  CrashServerGuard guard{srv, e.dir};
  ASSERT_TRUE(WaitReady(e.port)) << "server failed to start";

  auto read_stale = [&] {
    Client c(e.port);
    Reply r = c.Do("INFO", {});
    return (r.kind == Reply::Kind::Bulk) ? InfoMetric(r.str, "stale_replies") : 0u;
  };
  const uint64_t base = read_stale();

  const int N = 256;
  { Client c(e.port);
    for (int i = 0; i < N; ++i)
      ASSERT_EQ(c.Do("SET", {"ord_k" + std::to_string(i), "v" + std::to_string(i)}).kind,
                Reply::Kind::Simple); }

  for (int round = 0; round < 40; ++round) {
    PipelineClient pc(e.port);
    ASSERT_TRUE(pc.ok());
    for (int i = 0; i < N; ++i) pc.Add("GET", {"ord_k" + std::to_string(i)});
    ASSERT_TRUE(pc.Flush());
    std::vector<Reply> replies;
    ASSERT_TRUE(pc.ReadReplies(N, &replies));
    for (int i = 0; i < N; ++i) {
      ASSERT_EQ(replies[i].kind, Reply::Kind::Bulk);
      ASSERT_EQ(replies[i].str, "v" + std::to_string(i))
          << "reply out of order (round " << round << ", index " << i << ")";
    }
  }

  ASSERT_EQ(read_stale(), base) << "stale reply during pipeline regression";
}

TEST(CrashRecovery, PipelinedReplyOrderEverysec) { RunPipelineOrder("everysec"); }
TEST(CrashRecovery, PipelinedReplyOrderAlways)   { RunPipelineOrder("always"); }

// 混合 pipeline：一次 flush 混 ECHO/GET/EXISTS/SET/DEL，覆盖无 key / keyed / multi-key 三类。
TEST(CrashRecovery, MixedPipelineReplyOrder) {
  CycleEnv e = MakeCycleEnv("always");
  pid_t srv = SpawnServer(e.conf);
  CrashServerGuard guard{srv, e.dir};
  ASSERT_TRUE(WaitReady(e.port)) << "server failed to start";

  const int K = 64;
  { Client c(e.port);
    for (int i = 0; i < K; ++i)
      ASSERT_EQ(c.Do("SET", {"mk" + std::to_string(i), "v" + std::to_string(i)}).kind,
                Reply::Kind::Simple); }

  struct Step { std::string cmd; std::vector<std::string> args; Reply expect; };
  std::vector<Step> steps;
  for (int i = 0; i < K; ++i) {
    Reply b; b.kind = Reply::Kind::Bulk; b.str = "m" + std::to_string(i);
    steps.push_back({"ECHO", {"m" + std::to_string(i)}, b});
    b.str = "v" + std::to_string(i);
    steps.push_back({"GET", {"mk" + std::to_string(i)}, b});
    Reply n; n.kind = Reply::Kind::Int; n.integer = 1;
    steps.push_back({"EXISTS", {"mk" + std::to_string(i)}, n});
    Reply ok; ok.kind = Reply::Kind::Simple; ok.str = "OK";
    steps.push_back({"SET", {"mk" + std::to_string(i), "w" + std::to_string(i)}, ok});
    b.str = "w" + std::to_string(i);
    steps.push_back({"GET", {"mk" + std::to_string(i)}, b});
    n.integer = 1;
    steps.push_back({"DEL", {"mk" + std::to_string(i)}, n});
    n.integer = 0;
    steps.push_back({"EXISTS", {"mk" + std::to_string(i)}, n});
  }

  PipelineClient pc(e.port);
  ASSERT_TRUE(pc.ok());
  for (auto& s : steps) pc.Add(s.cmd, s.args);
  ASSERT_TRUE(pc.Flush());
  std::vector<Reply> replies;
  ASSERT_TRUE(pc.ReadReplies(steps.size(), &replies));
  ASSERT_EQ(replies.size(), steps.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    ASSERT_EQ(replies[i].kind, steps[i].expect.kind) << "kind mismatch at step " << i;
    if (steps[i].expect.kind == Reply::Kind::Bulk) {
      ASSERT_EQ(replies[i].str, steps[i].expect.str) << "bulk mismatch at step " << i;
    } else if (steps[i].expect.kind == Reply::Kind::Int) {
      ASSERT_EQ(replies[i].integer, steps[i].expect.integer) << "int mismatch at step " << i;
    }
  }
}

// Bug 2 回归：rewrite/bgsave 子进程 fork 后必须关闭继承的监听 socket，
// 否则父进程 kill -9 后端口仍被孤儿子进程占用，重启 EADDRINUSE。
TEST(FdInherit, RewriteChildNoSocketFd) { RunChildNoSocketFd("bgrewriteaof"); }
TEST(FdInherit, BgSaveChildNoSocketFd)   { RunChildNoSocketFd("bgsave"); }
TEST(FdInherit, RewritePortRebindAfterKill) { RunPortRebindAfterKill("bgrewriteaof"); }
TEST(FdInherit, BgSavePortRebindAfterKill)   { RunPortRebindAfterKill("bgsave"); }
