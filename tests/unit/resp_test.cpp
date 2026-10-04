#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include "mycache/net/buffer.h"
#include "mycache/protocol/resp_encoder.h"
#include "mycache/protocol/resp_parser.h"
#include "mycache/protocol/resp_value.h"

using namespace mycache;

namespace {

void ExpectParse(const std::string& input, RespValue* out) {
  RespParser p;
  size_t consumed = 0;
  ASSERT_EQ(p.Parse(input.data(), input.size(), &consumed, out), ParseResult::kOk);
  EXPECT_EQ(consumed, input.size());
  out->Materialize();  // 物化借引用，后续断言 v.str 才有值
}

// 断言输入触发协议错误（kError），不产生 UB、不崩溃。
void ExpectError(const std::string& input) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  EXPECT_EQ(p.Parse(input.data(), input.size(), &consumed, &v), ParseResult::kError);
}

// 把一条完整 RESP 串按 chunk 大小分片，逐片喂入一个 Buffer（模拟连接缓冲累积），
// 驱动解析器直到产出完整命令。chunk 任意切法（含 trailer 被拆到两次喂入）都应成功。
RespValue StreamParse(const std::string& data, size_t chunk) {
  Buffer buf;
  RespParser p;
  for (size_t i = 0; i < data.size(); i += chunk) {
    size_t n = std::min(chunk, data.size() - i);
    buf.append(data.data() + i, n);
    size_t consumed = 0;
    RespValue v;
    ParseResult r = p.Parse(buf.peek(), buf.readable(), &consumed, &v);
    buf.retrieve(consumed);
    if (r == ParseResult::kError) {
      ADD_FAILURE() << "parse error during streaming at offset " << i;
      return RespValue{};
    }
    if (r == ParseResult::kOk) { v.Materialize(); return v; }
  }
  ADD_FAILURE() << "command never completed";
  return RespValue{};
}

}  // namespace

TEST(RespParserTest, SimpleString) {
  RespValue v;
  ExpectParse("+OK\r\n", &v);
  EXPECT_EQ(v.type, RespType::kSimpleString);
  EXPECT_EQ(v.str, "OK");
}

TEST(RespParserTest, Error) {
  RespValue v;
  ExpectParse("-ERR nope\r\n", &v);
  EXPECT_EQ(v.type, RespType::kError);
  EXPECT_EQ(v.str, "ERR nope");
}

TEST(RespParserTest, Integer) {
  RespValue v;
  ExpectParse(":100\r\n", &v);
  EXPECT_EQ(v.type, RespType::kInteger);
  EXPECT_EQ(v.integer, 100);
}

TEST(RespParserTest, BulkString) {
  RespValue v;
  ExpectParse("$5\r\nhello\r\n", &v);
  EXPECT_EQ(v.type, RespType::kBulkString);
  EXPECT_EQ(v.str, "hello");
}

TEST(RespParserTest, EmptyBulk) {
  RespValue v;
  ExpectParse("$0\r\n\r\n", &v);
  EXPECT_EQ(v.type, RespType::kBulkString);
  EXPECT_EQ(v.str, "");
}

TEST(RespParserTest, NullBulk) {
  RespValue v;
  ExpectParse("$-1\r\n", &v);
  EXPECT_TRUE(v.IsNull());
}

// ---- 手写整数解析的边界（信任基础）----
// 覆盖溢出/非数字/空长度/超长数字，与 strtoll 行为一致：返回 kError 而非 UB。

TEST(RespParserTest, IntParseBulkLengthOverflow) {
  // 2^63 溢出 int64：返回 kError 而非 UB。
  ExpectError("$9223372036854775808\r\n");
  // 负方向溢出（|n| > 2^63）：同样 kError，与 $-1 的 null 语义区分开。
  ExpectError("$-9223372036854775809\r\n");
}

TEST(RespParserTest, IntParseArrayCountOverflow) {
  ExpectError("*99999999999999999999\r\n");
}

TEST(RespParserTest, IntParseNonDigit) {
  ExpectError("$abc\r\n");
  ExpectError("*12x\r\n");
}

TEST(RespParserTest, IntParseEmptyLengthLine) {
  // '$'/'*' 后直接 \r\n，长度行无数字 → 错误。
  ExpectError("$\r\n");
  ExpectError("*\r\n");
}

TEST(RespParserTest, IntParseHundredDigitString) {
  std::string digits(100, '9');
  ExpectError("$" + digits + "\r\n");
}

TEST(RespParserTest, NullBulkIsNotOverflow) {
  // $-1 是合法 null bulk，手写解析必须把它与「数字溢出」区分开。
  RespValue v;
  ExpectParse("$-1\r\n", &v);
  EXPECT_TRUE(v.IsNull());
}

TEST(RespParserTest, Array) {
  RespValue v;
  ExpectParse("*2\r\n$3\r\nget\r\n$3\r\nfoo\r\n", &v);
  ASSERT_EQ(v.type, RespType::kArray);
  ASSERT_EQ(v.array.size(), 2u);
  EXPECT_EQ(v.array[0].str, "get");
  EXPECT_EQ(v.array[1].str, "foo");
}

TEST(RespParserTest, IncompleteThenComplete) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // `*2\r\n$3\r\nget\r\n$3\r\nfoo\r\n` 拆成两段，跨 bulk body 断开。
  std::string part1 = "*2\r\n$3\r\nge";
  EXPECT_EQ(p.Parse(part1.data(), part1.size(), &consumed, &v), ParseResult::kIncomplete);
  EXPECT_EQ(consumed, part1.size());  // 半包时 consumed 也 > 0（已消费前缀）
  std::string part2 = "t\r\n$3\r\nfoo\r\n";
  EXPECT_EQ(p.Parse(part2.data(), part2.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, part2.size());
  v.Materialize();
  ASSERT_EQ(v.array.size(), 2u);
  EXPECT_EQ(v.array[0].str, "get");
  EXPECT_EQ(v.array[1].str, "foo");
}

TEST(RespParserTest, MultipleCommandsConsumeOneAtATime) {
  RespParser p;
  const std::string data = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
  size_t consumed = 0;
  RespValue v;
  ASSERT_EQ(p.Parse(data.data(), data.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, 14u);  // 只解析第一条命令
  EXPECT_EQ(v.array.size(), 1u);
}

TEST(RespParserTest, MalformedBulk) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  EXPECT_EQ(p.Parse("$5\r\nab\r\n", 8, &consumed, &v), ParseResult::kIncomplete);
}

TEST(RespParserTest, DeepNestingRejected) {
  // *1\r\n 无限嵌套会逐层压栈；深度上限应返回 kError 而非栈溢出。
  std::string input;
  constexpr int kDepth = 10000;
  for (int i = 0; i < kDepth; ++i) input += "*1\r\n";
  input += "$4\r\nPING\r\n";
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  EXPECT_EQ(p.Parse(input.data(), input.size(), &consumed, &v), ParseResult::kError);
}

TEST(RespParserTest, BulkTrailerSplit) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // body 已收完，但 trailer 只到了 '\r'：不消费 trailer，下次连同 '\n' 一起重查。
  std::string part1 = "$5\r\nhello\r";
  EXPECT_EQ(p.Parse(part1.data(), part1.size(), &consumed, &v), ParseResult::kIncomplete);
  EXPECT_EQ(consumed, 9u);  // 只消费到 body 结束，'\r' 留在缓冲
  std::string part2 = "\r\n";
  EXPECT_EQ(p.Parse(part2.data(), part2.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, 2u);
  v.Materialize();
  EXPECT_EQ(v.type, RespType::kBulkString);
  EXPECT_EQ(v.str, "hello");
}

TEST(RespParserTest, LargeArrayStreaming) {
  std::string data = "*1000\r\n";
  for (int i = 0; i < 1000; ++i) data += "$3\r\nfoo\r\n";

  for (size_t chunk : {size_t{1}, size_t{2}, size_t{7}, size_t{64}}) {
    RespValue v = StreamParse(data, chunk);
    ASSERT_EQ(v.type, RespType::kArray) << "chunk=" << chunk;
    ASSERT_EQ(v.array.size(), 1000u) << "chunk=" << chunk;
    EXPECT_EQ(v.array[0].str, "foo");
    EXPECT_EQ(v.array[999].str, "foo");
  }
}

TEST(RespParserTest, ResetAfterError) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // bulk 长度行不是数字 → 协议错误，consumed 保持 0。
  EXPECT_EQ(p.Parse("$abc\r\n", 6, &consumed, &v), ParseResult::kError);
  EXPECT_EQ(consumed, 0u);
  p.Reset();
  // Reset 后同一实例能正常解析新命令。
  EXPECT_EQ(p.Parse("+OK\r\n", 5, &consumed, &v), ParseResult::kOk);
  v.Materialize();
  EXPECT_EQ(v.type, RespType::kSimpleString);
  EXPECT_EQ(v.str, "OK");
}

TEST(RespParserTest, PipelineTwoCommands) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // 同一实例连续解析两条命令，中间状态无残留。
  EXPECT_EQ(p.Parse("*1\r\n$4\r\nPING\r\n", 14, &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, 14u);
  v.Materialize();
  EXPECT_EQ(v.array[0].str, "PING");
  EXPECT_EQ(p.Parse("*1\r\n$4\r\nPING\r\n", 14, &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, 14u);
  v.Materialize();
  EXPECT_EQ(v.array[0].str, "PING");
}

TEST(RespParserTest, LineTooLong) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // 无 '\n' 的超长行应被长度上限拦住，而非无限累积。
  std::string s = "+";
  s.append(65 * 1024, 'a');
  EXPECT_EQ(p.Parse(s.data(), s.size(), &consumed, &v), ParseResult::kError);
}

TEST(RespParserTest, SkipsInterCommandCrlf) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // 命令前的杂散 CR/LF（redis-cli --pipe 会追加结尾换行）应被跳过。
  std::string leading = "\r\n*1\r\n$4\r\nPING\r\n";
  EXPECT_EQ(p.Parse(leading.data(), leading.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, leading.size());
  v.Materialize();
  EXPECT_EQ(v.array[0].str, "PING");

  // 命令后的杂散 CR/LF 留在缓冲，下次 Parse 被跳过。
  std::string cmd = "*1\r\n$4\r\nPING\r\n";
  EXPECT_EQ(p.Parse(cmd.data(), cmd.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(consumed, cmd.size());
  std::string stray = "\r\n";
  EXPECT_EQ(p.Parse(stray.data(), stray.size(), &consumed, &v), ParseResult::kIncomplete);
  EXPECT_EQ(consumed, stray.size());
}

// ---- 零拷贝 view 语义（Step 3）----
// Parse 存借引用（view），物化推迟到拷贝构造 / 显式 Materialize。

TEST(RespParserTest, CopyMaterializesView) {
  // 模拟 keyed 路径：Parse 后 view 非空、str 空；args = cmd.array 拷贝触发物化。
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  std::string data = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  ASSERT_EQ(p.Parse(data.data(), data.size(), &consumed, &v), ParseResult::kOk);
  ASSERT_EQ(v.array.size(), 2u);
  EXPECT_NE(v.array[0].view.data(), nullptr);
  EXPECT_TRUE(v.array[0].str.empty());

  std::vector<RespValue> args = v.array;  // 拷贝构造器物化
  EXPECT_EQ(args[0].str, "foo");
  EXPECT_EQ(args[1].str, "bar");
  EXPECT_EQ(args[0].view.data(), nullptr);
}

TEST(RespParserTest, MovePreservesView) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  std::string data = "$3\r\nfoo\r\n";
  ASSERT_EQ(p.Parse(data.data(), data.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_NE(v.view.data(), nullptr);

  RespValue m = std::move(v);  // move 浅搬 view，不物化（零拷贝前提）
  EXPECT_NE(m.view.data(), nullptr);
  EXPECT_TRUE(m.str.empty());

  RespValue c = m;  // copy 才物化
  EXPECT_EQ(c.str, "foo");
  EXPECT_EQ(c.view.data(), nullptr);
}

TEST(RespParserTest, MoveAssignPreservesView) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  std::string data = "$3\r\nfoo\r\n";
  ASSERT_EQ(p.Parse(data.data(), data.size(), &consumed, &v), ParseResult::kOk);

  RespValue m;
  m = std::move(v);  // move 赋值浅搬 view
  EXPECT_NE(m.view.data(), nullptr);
  EXPECT_TRUE(m.str.empty());

  RespValue c = m;
  EXPECT_EQ(c.str, "foo");
}

TEST(RespParserTest, MaterializeClearsView) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  std::string data = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(p.Parse(data.data(), data.size(), &consumed, &v), ParseResult::kOk);
  EXPECT_NE(v.array[0].view.data(), nullptr);

  v.Materialize();
  EXPECT_EQ(v.array[0].str, "PING");
  EXPECT_EQ(v.array[0].view.data(), nullptr);  // 物化后 view 清空，强制不变量
}

TEST(RespParserTest, PartialBulkFallsBackToMaterialize) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  // body "hello" 拆两段：第一段不完整 → 回退 str.append，不存 view。
  EXPECT_EQ(p.Parse("$5\r\nhe", 6, &consumed, &v), ParseResult::kIncomplete);
  EXPECT_EQ(p.Parse("llo\r\n", 6, &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(v.str, "hello");
  EXPECT_EQ(v.view.data(), nullptr);  // 回退物化：view 为空
}

TEST(RespParserTest, EmptyBulkIsNotNullAfterCopy) {
  RespParser p;
  size_t consumed = 0;
  RespValue v;
  ASSERT_EQ(p.Parse("$0\r\n\r\n", 6, &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(v.type, RespType::kBulkString);
  // $0 走 kBulkCrlf（跳过 kBulkBody），view 未设置（data()==nullptr），str 空。
  EXPECT_EQ(v.view.data(), nullptr);
  EXPECT_TRUE(v.str.empty());

  RespValue c = v;  // 拷贝（Route）：空 bulk 仍是空串，不是 null 也不是垃圾
  EXPECT_EQ(c.type, RespType::kBulkString);
  EXPECT_EQ(c.str, "");
  EXPECT_EQ(c.view.data(), nullptr);
}

TEST(RespEncoderTest, RoundTrip) {
  EXPECT_EQ(EncodeResp(RespValue::Simple("PONG")), "+PONG\r\n");
  EXPECT_EQ(EncodeResp(RespValue::Bulk("hi")), "$2\r\nhi\r\n");
  EXPECT_EQ(EncodeResp(RespValue::Integer(1)), ":1\r\n");
  EXPECT_EQ(EncodeResp(RespValue::NullBulk()), "$-1\r\n");
  EXPECT_EQ(EncodeResp(RespValue::Error("ERR nope")), "-ERR nope\r\n");

  std::vector<RespValue> arr;
  arr.push_back(RespValue::Bulk("a"));
  arr.push_back(RespValue::Integer(2));
  EXPECT_EQ(EncodeResp(RespValue::Array(std::move(arr))), "*2\r\n$1\r\na\r\n:2\r\n");
}
