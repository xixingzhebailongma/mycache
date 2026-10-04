#include <cstdio>
#include <string>

#include "mycache/common/config.h"
#include "mycache/common/logger.h"
#include "mycache/server/server.h"
#include "mycache/storage/memory/memory_pool.h"

int main(int argc, char* argv[]) {
  std::string conf = "conf/mycache.conf";
  if (argc > 1) conf = argv[1];

  mycache::Config config;
  if (!config.Load(conf)) {
    std::fprintf(stderr, "failed to load config file: %s\n", conf.c_str());
    return 1;
  }

  mycache::LogLevel lv = mycache::LogLevel::kInfo;
  if (config.log_level == "debug") lv = mycache::LogLevel::kDebug;
  else if (config.log_level == "warn") lv = mycache::LogLevel::kWarn;
  else if (config.log_level == "error") lv = mycache::LogLevel::kError;
  mycache::Logger::Init(lv, config.log_file);

  mycache::Server server(config);
  server.Start();
  mycache::PoolFlushAll();  // main 线程空闲池归还 OS（used_memory 已由正常 zfree 归零）
  mycache::Logger::Shutdown();
  return 0;
}
