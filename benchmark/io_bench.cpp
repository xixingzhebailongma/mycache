// IOEngine write+fsync 压测：对比 posix 与 io_uring 的 write+fsync 吞吐（对应
// AOF appendfsync always 的负载）。用法：mycache_io_bench [records]
//
// 注意：当前 UringIO 是「同步提交 + 等待完成」的封装，单条 write+fsync 上并不会
// 明显快于 posix（io_uring 的真正收益在于批提交多个 SQE）。此压测用于验证 io_uring
// 路径可用并给出数量级参考，而非宣称它更快。
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "mycache/io/io_engine.h"

int main(int argc, char** argv) {
  int n = 20000;
  if (argc > 1) n = std::atoi(argv[1]);
  if (n <= 0) n = 20000;

  const std::string rec(64, 'x');  // 64 字节记录
  const char* kEngines[] = {"posix", "io_uring"};

  std::printf("IOEngine write+fsync benchmark: %d records per engine\n\n", n);
  std::printf("%-9s %10s %12s %14s\n", "engine", "records", "time(ms)", "ops/s");

  for (const char* requested : kEngines) {
    auto io = mycache::CreateIOEngine(requested);
    std::string path = std::string("/tmp/mycache_io_bench_") + requested + ".dat";
    int fd = io->Open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      std::printf("%-9s open failed\n", requested);
      continue;
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
      io->Write(fd, rec.data(), rec.size());
      io->Fsync(fd);
    }
    auto t1 = std::chrono::steady_clock::now();
    io->Close(fd);
    ::unlink(path.c_str());

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("%-9s %10d %12.1f %14.0f\n", io->Name(), n, ms, n / (ms / 1000.0));
  }
  return 0;
}
