#include <string>

#include <gtest/gtest.h>

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
  EXPECT_EQ(p.Parse("*2\r\n$3\r\nge", 10, &consumed, &v), ParseResult::kIncomplete);
  EXPECT_EQ(consumed, 0u);
  EXPECT_EQ(p.Parse("*2\r\n$3\r\nget\r\n$3\r\nfoo\r\n", 24, &consumed, &v), ParseResult::kOk);
  EXPECT_EQ(v.array.size(), 2u);
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
