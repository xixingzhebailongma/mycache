#include "mycache/protocol/resp_parser.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace mycache {

namespace {

constexpr int64_t kMaxBulkLength = 512 * 1024 * 1024;  // 512MB，与 Redis 一致
constexpr int64_t kMaxArrayElements = 1024 * 1024;

// 读取以 CRLF（或单独的 LF）结尾的一行；将 *pos 推进到换行符之后。
ParseResult ReadLine(const char* data, size_t len, size_t* pos, std::string* line) {
  const void* nl = std::memchr(data + *pos, '\n', len - *pos);
  if (nl == nullptr) return ParseResult::kIncomplete;
  size_t newline = static_cast<const char*>(nl) - data;
  size_t line_start = *pos;
  size_t line_len = newline - line_start;
  if (line_len > 0 && data[line_start + line_len - 1] == '\r') --line_len;
  line->assign(data + line_start, line_len);
  *pos = newline + 1;
  return ParseResult::kOk;
}

bool ParseInt64(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

}  // namespace

ParseResult RespParser::Parse(const char* data, size_t len, size_t* consumed, RespValue* out) {
  size_t pos = 0;
  *out = RespValue{};
  ParseResult r = ParseOne(data, len, &pos, out);
  *consumed = (r == ParseResult::kOk) ? pos : 0;
  return r;
}

ParseResult RespParser::ParseOne(const char* data, size_t len, size_t* pos, RespValue* out) {
  if (*pos >= len) return ParseResult::kIncomplete;
  char type = data[(*pos)++];
  std::string line;
  int64_t num = 0;

  switch (type) {
    case '+':
    case '-': {
      ParseResult r = ReadLine(data, len, pos, &line);
      if (r != ParseResult::kOk) return r;
      out->type = (type == '+') ? RespType::kSimpleString : RespType::kError;
      out->str = std::move(line);
      return ParseResult::kOk;
    }
    case ':': {
      ParseResult r = ReadLine(data, len, pos, &line);
      if (r != ParseResult::kOk) return r;
      if (!ParseInt64(line, &num)) return ParseResult::kError;
      out->type = RespType::kInteger;
      out->integer = num;
      return ParseResult::kOk;
    }
    case '$': {
      ParseResult r = ReadLine(data, len, pos, &line);
      if (r != ParseResult::kOk) return r;
      if (!ParseInt64(line, &num)) return ParseResult::kError;
      if (num == -1) {
        out->type = RespType::kNull;
        return ParseResult::kOk;
      }
      if (num < -1 || num > kMaxBulkLength) return ParseResult::kError;
      size_t n = static_cast<size_t>(num);
      if (*pos + n + 2 > len) return ParseResult::kIncomplete;
      out->type = RespType::kBulkString;
      out->str.assign(data + *pos, n);
      *pos += n;
      if (data[*pos] != '\r' || data[*pos + 1] != '\n') return ParseResult::kError;
      *pos += 2;
      return ParseResult::kOk;
    }
    case '*': {
      ParseResult r = ReadLine(data, len, pos, &line);
      if (r != ParseResult::kOk) return r;
      if (!ParseInt64(line, &num)) return ParseResult::kError;
      if (num == -1) {
        out->type = RespType::kNull;
        return ParseResult::kOk;
      }
      if (num < -1 || num > kMaxArrayElements) return ParseResult::kError;
      out->type = RespType::kArray;
      out->array.clear();
      out->array.reserve(static_cast<size_t>(num));
      for (int64_t i = 0; i < num; ++i) {
        RespValue elem;
        ParseResult er = ParseOne(data, len, pos, &elem);
        if (er != ParseResult::kOk) return er;
        out->array.push_back(std::move(elem));
      }
      return ParseResult::kOk;
    }
    default:
      return ParseResult::kError;
  }
}

}  // namespace mycache
