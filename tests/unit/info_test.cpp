#include <string>

#include <gtest/gtest.h>

#include "mycache/common/config.h"
#include "mycache/eviction/eviction_policy.h"
#include "mycache/monitor/info.h"
#include "mycache/server/command_router.h"

TEST(ParseMemorySizeTest, BytesAndSuffixes) {
  int64_t v = 0;
  EXPECT_TRUE(mycache::ParseMemorySize("100", &v));
  EXPECT_EQ(v, 100);
  EXPECT_TRUE(mycache::ParseMemorySize("1k", &v));
  EXPECT_EQ(v, 1000);
  EXPECT_TRUE(mycache::ParseMemorySize("1kb", &v));
  EXPECT_EQ(v, 1024);
  EXPECT_TRUE(mycache::ParseMemorySize("2mb", &v));
  EXPECT_EQ(v, 2 * 1024 * 1024);
  EXPECT_TRUE(mycache::ParseMemorySize("1g", &v));
  EXPECT_EQ(v, 1000000000);
  EXPECT_FALSE(mycache::ParseMemorySize("abc", &v));
  EXPECT_FALSE(mycache::ParseMemorySize("-1", &v));
}

TEST(InfoTest, BuildInfoContainsCoreFields) {
  mycache::Config config;
  config.maxmemory = 1024;
  config.maxmemory_policy = "allkeys-lru";
  mycache::CommandRouter router(2, mycache::EvictionConfig{});

  std::string info = mycache::BuildInfo(config, router);
  EXPECT_NE(info.find("used_memory:"), std::string::npos);
  EXPECT_NE(info.find("maxmemory:1024"), std::string::npos);
  EXPECT_NE(info.find("maxmemory_policy:allkeys-lru"), std::string::npos);
  EXPECT_NE(info.find("mem_fragmentation_ratio:"), std::string::npos);
  EXPECT_NE(info.find("keyspace_hits:"), std::string::npos);
  EXPECT_NE(info.find("db0:keys=0"), std::string::npos);
}
