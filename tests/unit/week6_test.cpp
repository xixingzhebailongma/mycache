#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mycache/common/config.h"
#include "mycache/common/time.h"
#include "mycache/eviction/eviction_policy.h"
#include "mycache/io/io_engine.h"
#include "mycache/persistence/aof/aof_recovery.h"
#include "mycache/persistence/aof/aof_rewrite.h"
#include "mycache/persistence/aof/aof_writer.h"
#include "mycache/persistence/persistence_manager.h"
#include "mycache/persistence/rdb/rdb_loader.h"
#include "mycache/persistence/rdb/rdb_writer.h"
#include "mycache/server/command_router.h"
#include "mycache/storage/db/shard.h"

namespace mycache {
namespace {

std::string TempPath(const char* tag) {
  return std::string("/tmp/mycache_week6_") + tag + "_" + std::to_string(::getpid()) + ".tmp";
}

// 按 key 的哈希路由到对应 shard，单线程直接写/读（测试用，worker 未启动）。
void SetKey(CommandRouter* r, const std::string& k, const std::string& v) {
  r->ShardAt(r->ShardIndex(k))->Set(k, v);
}
bool GetKey(CommandRouter* r, const std::string& k, std::string* v) {
  return r->ShardAt(r->ShardIndex(k))->Get(k, v);
}

}  // namespace

TEST(Rdb, RoundTripStringHashListTtl) {
  EvictionConfig ev;
  CommandRouter r1(4, ev);

  SetKey(&r1, "foo", "bar");
  SetKey(&r1, "num", "12345");
  r1.ShardAt(r1.ShardIndex("h"))->HSet("h", "f1", "v1");
  r1.ShardAt(r1.ShardIndex("h"))->HSet("h", "f2", "v2");
  r1.ShardAt(r1.ShardIndex("list"))->RPush("list", {"a", "b", "c"});
  r1.ShardAt(r1.ShardIndex("foo"))->ExpireAt("foo", UnixMs() + 60 * 1000);

  std::string path = TempPath("rdb");
  std::vector<char> out(kRdbOutCap), rec(kRdbRecCap);
  ASSERT_TRUE(RdbWriteSnapshot(&r1, path.c_str(), out.data(), out.size(), rec.data(), rec.size()));

  CommandRouter r2(4, ev);
  RdbLoadResult lr = RdbLoad(path, &r2);
  ASSERT_TRUE(lr.ok) << lr.error;
  EXPECT_EQ(lr.keys_loaded, 4u);

  std::string v;
  ASSERT_TRUE(GetKey(&r2, "foo", &v));
  EXPECT_EQ(v, "bar");
  ASSERT_TRUE(GetKey(&r2, "num", &v));
  EXPECT_EQ(v, "12345");
  ASSERT_TRUE(r2.ShardAt(r2.ShardIndex("h"))->HGet("h", "f1", &v));
  EXPECT_EQ(v, "v1");
  ASSERT_TRUE(r2.ShardAt(r2.ShardIndex("h"))->HGet("h", "f2", &v));
  EXPECT_EQ(v, "v2");
  auto l = r2.ShardAt(r2.ShardIndex("list"))->LRange("list", 0, -1);
  ASSERT_EQ(l.size(), 3u);
  EXPECT_EQ(l[0], "a");
  EXPECT_EQ(l[1], "b");
  EXPECT_EQ(l[2], "c");
  EXPECT_GT(r2.ShardAt(r2.ShardIndex("foo"))->TtlMs("foo"), 0);

  ::unlink(path.c_str());
}

TEST(Rdb, CorruptedRecordFails) {
  EvictionConfig ev;
  CommandRouter r1(2, ev);
  SetKey(&r1, "foo", "bar");
  SetKey(&r1, "baz", "qux");

  std::string path = TempPath("rdbbad");
  std::vector<char> out(kRdbOutCap), rec(kRdbRecCap);
  ASSERT_TRUE(RdbWriteSnapshot(&r1, path.c_str(), out.data(), out.size(), rec.data(), rec.size()));

  // 读回文件，翻转中间一个字节，破坏一条记录的 CRC。
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_GT(bytes.size(), kRdbMagicSize + 1u);
  bytes[kRdbMagicSize + 1 + 4] ^= 0xFF;  // 破坏第一条记录的 payload 首字节
  {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  CommandRouter r2(2, ev);
  RdbLoadResult lr = RdbLoad(path, &r2);
  EXPECT_FALSE(lr.ok);
  ::unlink(path.c_str());
}

TEST(AofRewrite, ChildProducesReplayableLog) {
  EvictionConfig ev;
  CommandRouter r1(4, ev);

  SetKey(&r1, "foo", "bar");
  SetKey(&r1, "num", "98765");
  r1.ShardAt(r1.ShardIndex("h"))->HSet("h", "f1", "v1");
  r1.ShardAt(r1.ShardIndex("list"))->RPush("list", {"x", "y", "z"});
  r1.ShardAt(r1.ShardIndex("foo"))->ExpireAt("foo", UnixMs() + 60 * 1000);

  std::string path = TempPath("rewrite");
  std::vector<char> out(kRewriteOutCap), rec(kRewriteRecCap);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    RewriteAofChild(&r1, path.c_str(), out.data(), out.size(), rec.data(), rec.size());
    _exit(1);  // 不应到达
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  CommandRouter r2(4, ev);
  AofRecoveryResult rr = AofRecovery::Replay(
      path, [&r2](const std::string& name, const std::vector<std::string>& args) {
        if (args.empty()) return false;
        return ApplyAofCommand(r2.ShardAt(r2.ShardIndex(args[0])), name, args);
      });
  ASSERT_TRUE(rr.ok) << rr.error;

  std::string v;
  ASSERT_TRUE(GetKey(&r2, "foo", &v));
  EXPECT_EQ(v, "bar");
  ASSERT_TRUE(GetKey(&r2, "num", &v));
  EXPECT_EQ(v, "98765");
  ASSERT_TRUE(r2.ShardAt(r2.ShardIndex("h"))->HGet("h", "f1", &v));
  EXPECT_EQ(v, "v1");
  auto l = r2.ShardAt(r2.ShardIndex("list"))->LRange("list", 0, -1);
  ASSERT_EQ(l.size(), 3u);
  EXPECT_EQ(l[0], "x");
  EXPECT_EQ(l[1], "y");
  EXPECT_EQ(l[2], "z");
  EXPECT_GT(r2.ShardAt(r2.ShardIndex("foo"))->TtlMs("foo"), 0);

  ::unlink(path.c_str());
}

TEST(AofRecovery, ListCommandsReplay) {
  Shard shard;
  EXPECT_TRUE(ApplyAofCommand(&shard, "rpush", {"l", "a", "b", "c"}));
  EXPECT_TRUE(ApplyAofCommand(&shard, "lpush", {"l", "x"}));
  EXPECT_TRUE(ApplyAofCommand(&shard, "lpop", {"l"}));

  auto vals = shard.LRange("l", 0, -1);
  ASSERT_EQ(vals.size(), 3u);
  EXPECT_EQ(vals[0], "a");
  EXPECT_EQ(vals[1], "b");
  EXPECT_EQ(vals[2], "c");
}

TEST(IoEngine, FactoryFallsBackToPosix) {
  auto posix = CreateIOEngine("posix");
  ASSERT_NE(posix, nullptr);
  EXPECT_STREQ(posix->Name(), "posix");

  auto bogus = CreateIOEngine("bogus");
  ASSERT_NE(bogus, nullptr);
  EXPECT_STREQ(bogus->Name(), "posix");

  auto uring = CreateIOEngine("io_uring");
  ASSERT_NE(uring, nullptr);
  // 未编译 io_uring 时回退 posix；编译后为 io_uring。两种都必须可用。
}

TEST(IoEngine, PosixWritePreadRoundTrip) {
  auto io = CreateIOEngine("posix");
  std::string path = TempPath("io");
  ::unlink(path.c_str());

  int fd = io->Open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(io->Write(fd, "hello", 5), 5);
  EXPECT_EQ(io->Fsync(fd), 0);
  io->Close(fd);

  fd = io->Open(path, O_RDONLY, 0);
  ASSERT_GE(fd, 0);
  char buf[8] = {0};
  EXPECT_EQ(io->Pread(fd, buf, 5, 0), 5);
  EXPECT_STREQ(buf, "hello");
  io->Close(fd);

  ::unlink(path.c_str());
}

// ---- 自动 AOF rewrite 触发（Week 6 收尾）----

TEST(AutoRewrite, ShouldAutoRewriteBoundaries) {
  // percentage <= 0：关闭自动 rewrite（含负值）。
  EXPECT_FALSE(ShouldAutoRewrite(10 * 1024 * 1024, 1024, 64 * 1024 * 1024, 0));
  EXPECT_FALSE(ShouldAutoRewrite(10 * 1024 * 1024, 1024, 64 * 1024 * 1024, -50));

  // 未达到 min_size：不触发。
  EXPECT_FALSE(ShouldAutoRewrite(1024, 1, 2048, 100));

  // current <= base：未增长，不触发。
  EXPECT_FALSE(ShouldAutoRewrite(1000, 1000, 1, 100));
  EXPECT_FALSE(ShouldAutoRewrite(500, 1000, 1, 100));

  // 增长不足 percentage：不触发。base=1000→current=1500 增长 50% < 100%。
  EXPECT_FALSE(ShouldAutoRewrite(1500, 1000, 1, 100));

  // 恰等于 percentage：触发。base=1000→current=2000 增长 100%。
  EXPECT_TRUE(ShouldAutoRewrite(2000, 1000, 1, 100));

  // base == 0：视为 1，任何 current>min 的增长都触发。
  EXPECT_TRUE(ShouldAutoRewrite(100, 0, 1, 100));
}

TEST(AutoRewrite, MaybeAutoRewriteSilentlySkipsWithoutAof) {
  PersistenceManager pm;  // 未 Init：aof_ 为空、aof_enabled_ 为 false
  EXPECT_FALSE(pm.MaybeAutoRewrite());
}

TEST(AutoRewrite, MaybeAutoRewriteSkipsWhileRewriting) {
  Config cfg;
  cfg.appendonly = true;
  cfg.appendfsync = "no";  // 不启动后台 flush 线程
  std::string aof_path = "/tmp/mycache_auto_rewrite_" + std::to_string(::getpid()) + ".aof";
  cfg.appendfilename = aof_path;  // dir 空 → AofPath() 直接返回绝对路径
  ::unlink(aof_path.c_str());

  EvictionConfig ev;
  CommandRouter router(4, ev);
  PersistenceManager pm;
  ASSERT_TRUE(pm.Init(cfg, &router));

  // 手动进入 rewrite 状态（rewriting_=true）：自动触发应静默跳过，不 fork。
  pm.aof()->BeginRewrite();
  EXPECT_FALSE(pm.MaybeAutoRewrite());
  pm.aof()->AbortRewrite();

  pm.Close();
  ::unlink(aof_path.c_str());
}

}  // namespace mycache
