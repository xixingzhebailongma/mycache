#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mycache/common/time.h"
#include "mycache/persistence/aof/aof_format.h"
#include "mycache/persistence/aof/aof_recovery.h"
#include "mycache/persistence/aof/aof_writer.h"
#include "mycache/storage/db/shard.h"

using mycache::AofFsync;
using mycache::AofRecovery;
using mycache::AofRecoveryResult;
using mycache::AofWriter;
using mycache::Shard;
namespace aof = mycache::aof;

namespace {

// 在 /tmp 下生成唯一的测试文件路径，测试结束用 unlink 清理。
std::string TempPath(const char* tag) {
  return std::string("/tmp/mycache_aof_") + tag + "_" + std::to_string(::getpid()) + ".aof";
}

// 把字节串直接写成一个 AOF 文件。
void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// 单 shard 的重放辅助：所有命令落到同一个 shard。
AofRecoveryResult ReplayInto(const std::string& path, Shard* shard) {
  return AofRecovery::Replay(
      path, [shard](const std::string& name, const std::vector<std::string>& args) {
        return mycache::ApplyAofCommand(shard, name, args);
      });
}

}  // namespace

TEST(AofFormat, Crc32KnownValue) {
  // "123456789" 的标准 CRC32 为 0xCBF43926。
  EXPECT_EQ(aof::Crc32("123456789", 9), 0xCBF43926u);
}

TEST(AofFormat, EncodeRecordHeaderRoundTrip) {
  std::string payload = aof::EncodeCommand("SET", {"k", "v"});
  std::string rec = aof::EncodeRecord(payload);
  EXPECT_EQ(rec.size(), 8u + payload.size());
  // 小端 length 前缀应等于 payload 大小。
  const auto* b = reinterpret_cast<const unsigned char*>(rec.data());
  uint32_t len = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
  EXPECT_EQ(len, static_cast<uint32_t>(payload.size()));
}

TEST(AofWriter, RoundTripThroughFile) {
  std::string path = TempPath("roundtrip");
  ::unlink(path.c_str());

  {
    AofWriter w;
    ASSERT_TRUE(w.Open(path, AofFsync::kAlways));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("SET", {"k1", "v1"})));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("HSET", {"h", "f", "x"})));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("SET", {"k2", "v2"})));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("DEL", {"k2"})));
    w.Close();
  }

  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.commands_replayed, 4u);
  EXPECT_EQ(r.bytes_truncated, 0u);

  std::string v;
  ASSERT_TRUE(shard.Get("k1", &v));
  EXPECT_EQ(v, "v1");
  EXPECT_FALSE(shard.Get("k2", &v));  // 已删除
  ASSERT_TRUE(shard.HGet("h", "f", &v));
  EXPECT_EQ(v, "x");

  ::unlink(path.c_str());
}

TEST(AofRecovery, TtlPexpireatSurvivesReplay) {
  std::string path = TempPath("ttl");
  ::unlink(path.c_str());

  int64_t when = mycache::UnixMs() + 60 * 1000;  // 60 秒后到期
  {
    AofWriter w;
    ASSERT_TRUE(w.Open(path, AofFsync::kNo));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("SET", {"k", "v"})));
    w.Append(aof::EncodeRecord(aof::EncodeCommand("PEXPIREAT", {"k", std::to_string(when)})));
    w.Close();
  }

  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok) << r.error;

  std::string v;
  ASSERT_TRUE(shard.Get("k", &v));
  EXPECT_EQ(v, "v");
  int64_t ttl = shard.TtlMs("k");
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 60 * 1000);  // 与写入时的绝对时间一致
  ::unlink(path.c_str());
}

TEST(AofRecovery, PartialTailIsTruncated) {
  std::string path = TempPath("partial");
  ::unlink(path.c_str());

  // 两条完整记录 + 一条被 kill -9 截断的半条记录。
  std::string file;
  file.append(aof::kMagic, aof::kMagicSize);
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"a", "1"}));
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"b", "2"}));
  std::string partial = aof::EncodeRecord(aof::EncodeCommand("SET", {"c", "3"}));
  partial.resize(partial.size() - 5);  // 截断尾部 5 字节
  file += partial;
  WriteFile(path, file);

  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.commands_replayed, 2u);
  EXPECT_EQ(r.bytes_truncated, partial.size());  // 整条不完整记录被丢弃

  std::string v;
  EXPECT_TRUE(shard.Get("a", &v));
  EXPECT_TRUE(shard.Get("b", &v));
  EXPECT_FALSE(shard.Get("c", &v));  // 半条记录被丢弃

  // 文件已被修复：长度等于有效前缀（原始长度 - 被丢弃的半条记录）。
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  EXPECT_EQ(static_cast<size_t>(in.tellg()), file.size() - partial.size());
  ::unlink(path.c_str());
}

TEST(AofRecovery, PartialHeaderIsTruncated) {
  std::string path = TempPath("partialhdr");
  ::unlink(path.c_str());

  std::string file;
  file.append(aof::kMagic, aof::kMagicSize);
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"a", "1"}));
  file += std::string("\x10\x00\x00", 3);  // 只有 3 字节的不完整记录头
  WriteFile(path, file);

  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.commands_replayed, 1u);
  EXPECT_EQ(r.bytes_truncated, 3u);
  ::unlink(path.c_str());
}

TEST(AofRecovery, MiddleChecksumMismatchFails) {
  std::string path = TempPath("badmid");
  ::unlink(path.c_str());

  std::string file;
  file.append(aof::kMagic, aof::kMagicSize);
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"a", "1"}));
  std::string rec2 = aof::EncodeRecord(aof::EncodeCommand("SET", {"b", "2"}));
  rec2[8 + 5] ^= 0xFF;  // 翻转中间记录载荷的一个字节
  file += rec2;
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"c", "3"}));
  WriteFile(path, file);

  Shard shard;
  auto r = ReplayInto(path, &shard);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("checksum"), std::string::npos);
  ::unlink(path.c_str());
}

TEST(AofRecovery, LastRecordCorruptionTruncatesAsTail) {
  std::string path = TempPath("badlast");
  ::unlink(path.c_str());

  std::string file;
  file.append(aof::kMagic, aof::kMagicSize);
  file += aof::EncodeRecord(aof::EncodeCommand("SET", {"a", "1"}));
  std::string rec2 = aof::EncodeRecord(aof::EncodeCommand("SET", {"b", "2"}));
  rec2[8] ^= 0xFF;  // 损坏最后一条记录
  file += rec2;
  WriteFile(path, file);

  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.commands_replayed, 1u);
  EXPECT_EQ(r.bytes_truncated, rec2.size());
  std::string v;
  EXPECT_TRUE(shard.Get("a", &v));
  EXPECT_FALSE(shard.Get("b", &v));
  ::unlink(path.c_str());
}

TEST(AofRecovery, MissingFileIsEmptyAof) {
  std::string path = TempPath("missing");
  ::unlink(path.c_str());
  Shard shard;
  auto r = ReplayInto(path, &shard);
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.commands_replayed, 0u);
  EXPECT_EQ(r.bytes_truncated, 0u);
}

TEST(AofRecovery, BadMagicFails) {
  std::string path = TempPath("badmagic");
  ::unlink(path.c_str());
  WriteFile(path, std::string("NOTANAOF", 8));
  Shard shard;
  auto r = ReplayInto(path, &shard);
  EXPECT_FALSE(r.ok);
  ::unlink(path.c_str());
}

TEST(AofFsync, ParsePolicy) {
  AofFsync p;
  ASSERT_TRUE(mycache::ParseAofFsync("always", &p));
  EXPECT_EQ(p, AofFsync::kAlways);
  ASSERT_TRUE(mycache::ParseAofFsync("everysec", &p));
  EXPECT_EQ(p, AofFsync::kEverysec);
  ASSERT_TRUE(mycache::ParseAofFsync("no", &p));
  EXPECT_EQ(p, AofFsync::kNo);
  EXPECT_FALSE(mycache::ParseAofFsync("sometimes", &p));
}

TEST(AofWriter, EmptyFileGetsHeader) {
  std::string path = TempPath("header");
  ::unlink(path.c_str());
  {
    AofWriter w;
    ASSERT_TRUE(w.Open(path, AofFsync::kNo));
    w.Close();
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  EXPECT_EQ(static_cast<size_t>(in.tellg()), aof::kMagicSize);
  ::unlink(path.c_str());
}

TEST(AofWriter, CompleteRewriteResetsBaseSize) {
  std::string path = TempPath("rewritebase");
  std::string temp = path + ".tmp";
  ::unlink(path.c_str());
  ::unlink(temp.c_str());

  {
    AofWriter w;
    ASSERT_TRUE(w.Open(path, AofFsync::kNo));
    uint64_t base0 = w.base_size();
    EXPECT_EQ(base0, w.current_size());  // 打开后 base == current（magic 头）

    w.Append(aof::EncodeRecord(aof::EncodeCommand("SET", {"a", "1"})));
    EXPECT_GT(w.current_size(), base0);  // append 只增 current
    EXPECT_EQ(w.base_size(), base0);     // base 不动

    w.BeginRewrite();
    EXPECT_EQ(w.base_size(), base0);     // 修复点：BeginRewrite 不再改 base

    // 子进程产出的 compact temp：本测试只关心尺寸与 rename，内容任意。
    std::string compact(64, 'x');
    WriteFile(temp, compact);

    // rewrite 期间写入：进 rewrite_buffer_，CompleteRewrite 会补到 temp 末尾。
    std::string buf_rec = aof::EncodeRecord(aof::EncodeCommand("SET", {"b", "2"}));
    w.Append(buf_rec);

    ASSERT_TRUE(w.CompleteRewrite(temp));

    // 修复点：rewrite 完成后 base == current ==（compact 尺寸 + buffer 尺寸）。
    uint64_t expect = compact.size() + buf_rec.size();
    EXPECT_EQ(w.current_size(), expect);
    EXPECT_EQ(w.base_size(), expect);
  }

  ::unlink(path.c_str());
}
