// AOF 三种 fsync 策略压测：绕过网络，直接驱动 AofWriter 追加记录，
// 对比 always / everysec / no 的吞吐。用法：mycache_aof_bench [records]
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

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

  std::printf("\nnote: 'always' fsyncs every record; 'everysec' fsyncs once per\n"
              "second; 'no' never fsyncs. All three write() to the OS page cache\n"
              "on every append, so kill -9 does not lose already-acknowledged data.\n");
  return 0;
}
