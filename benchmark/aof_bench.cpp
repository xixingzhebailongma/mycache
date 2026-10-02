// AOF 三种 fsync 策略压测：绕过网络，直接驱动 AofWriter 追加记录，
// 对比 always / everysec / no 的吞吐。用法：mycache_aof_bench [records]
//
// 追加：并发组提交压测（always 档，8 线程各自 append，join 后按总记录数算 ops/s），
// 对比 posix 组提交 vs io_uring 组提交（linked SQE）。
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "mycache/io/io_engine.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/persistence/aof/aof_writer.h"

using mycache::AofFsync;
using mycache::AofWriter;

namespace {

struct PolicySpec {
  const char* name;
  AofFsync policy;
};

const PolicySpec kPolicies[] = {
    {"always", AofFsync::kAlways},
    {"everysec", AofFsync::kEverysec},
    {"no", AofFsync::kNo},
};

// 并发组提交压测：kThreads 线程各自 append，join 后按总记录数算 ops/s。
// engine 传入 "posix" / "io_uring"；io->Name() 反映实际引擎（io_uring 初始化
// 失败会自动回退 posix，行首标签据此如实体现实情）。
void BenchAlwaysConcurrent(const char* engine, int n) {
  constexpr int kThreads = 8;
  const int per = n / kThreads;
  if (per <= 0) return;

  auto io = mycache::CreateIOEngine(engine);
  const std::string rec = mycache::aof::EncodeRecord(mycache::aof::EncodeCommand(
      "SET", {"bench:key", "0123456789abcdef0123456789abcdef"}));
  const std::string path =
      std::string("/tmp/mycache_aof_bench_conc_") + engine + ".aof";
  ::unlink(path.c_str());

  AofWriter w(io.get());
  if (!w.Open(path, AofFsync::kAlways)) {
    std::fprintf(stderr, "failed to open %s\n", path.c_str());
    return;
  }

  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int j = 0; j < per; ++j) w.Append(rec);
    });
  }

  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : threads) t.join();
  const auto t1 = std::chrono::steady_clock::now();
  w.Close();

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const int total = per * kThreads;
  const double ops = (ms > 0) ? total / (ms / 1000.0) : 0.0;
  std::printf("%-9s %10d %12.1f %14.0f\n",
              (std::string("c") + std::to_string(kThreads) + "-" + io->Name()).c_str(),
              total, ms, ops);
  ::unlink(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  int n = 50000;
  if (argc > 1) n = std::atoi(argv[1]);
  if (n <= 0) n = 50000;

  std::printf("AOF fsync policy benchmark: %d records per policy\n\n", n);
  std::printf("%-9s %10s %12s %14s\n", "policy", "records", "time(ms)", "ops/s");

  for (const auto& spec : kPolicies) {
    std::string path = std::string("/tmp/mycache_aof_bench_") + spec.name + ".aof";
    ::unlink(path.c_str());

    AofWriter w;
    if (!w.Open(path, spec.policy)) {
      std::fprintf(stderr, "failed to open %s\n", path.c_str());
      return 1;
    }

    const std::string rec =
        mycache::aof::EncodeRecord(mycache::aof::EncodeCommand(
            "SET", {"bench:key", "0123456789abcdef0123456789abcdef"}));

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) w.Append(rec);
    const auto t1 = std::chrono::steady_clock::now();
    w.Close();

    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double ops = (ms > 0) ? n / (ms / 1000.0) : 0.0;
    std::printf("%-9s %10d %12.1f %14.0f\n", spec.name, n, ms, ops);
    ::unlink(path.c_str());
  }

  std::printf("\nconcurrent (always, 8 threads):\n");
  std::printf("%-9s %10s %12s %14s\n", "policy", "records", "time(ms)", "ops/s");
  BenchAlwaysConcurrent("posix", n);
  BenchAlwaysConcurrent("io_uring", n);

  std::printf("\nnote: 'always' fsyncs every record; 'everysec' fsyncs once per\n"
              "second; 'no' never fsyncs. All three write() to the OS page cache\n"
              "on every append, so kill -9 does not lose already-acknowledged data.\n");
  return 0;
}
