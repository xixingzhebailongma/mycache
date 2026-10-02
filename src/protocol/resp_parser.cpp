#include "mycache/protocol/resp_parser.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <string>
#include <utility>

namespace mycache {

namespace {

constexpr int64_t kMaxBulkLength = 512 * 1024 * 1024;  // 512MB，与 Redis 一致
constexpr int64_t kMaxArrayElements = 1024 * 1024;
constexpr int kMaxNestingDepth = 128;         // 嵌套深度上限，超限视为协议错误
constexpr size_t kMaxLineLength = 64 * 1024;  // 行长度上限，防无 '\n' 时无限增长

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

void RespParser::Reset() {
  step_ = Step::kType;
  line_kind_ = LineKind::kSimple;
  stack_.clear();
  current_ = RespValue{};
  bulk_remaining_ = 0;
  line_.clear();
}

bool RespParser::Commit(RespValue value, RespValue* out) {
  RespValue cur = std::move(value);
  while (true) {
    if (stack_.empty()) {
      *out = std::move(cur);
      return true;
    }
    Frame& f = stack_.back();
    f.value.array.push_back(std::move(cur));
    if (--f.remaining > 0) return false;
    cur = std::move(f.value);
    stack_.pop_back();
  }
}

ParseResult RespParser::Parse(const char* data, size_t len, size_t* consumed, RespValue* out) {
  size_t pos = 0;
  *consumed = 0;

  while (true) {
    switch (step_) {
      case Step::kType: {
        if (pos >= len) {
          *consumed = pos;
          return ParseResult::kIncomplete;
        }
        char type = data[pos++];
        switch (type) {
          case '\r':
          case '\n':
            // 命令之间的杂散 CR/LF（如 redis-cli --pipe 追加的结尾换行）：跳过。
            // 停留 kType，继续读下一个字节。
            break;
          case '+':
            line_kind_ = LineKind::kSimple;
            step_ = Step::kLine;
            break;
          case '-':
            line_kind_ = LineKind::kError;
            step_ = Step::kLine;
            break;
          case ':':
            line_kind_ = LineKind::kInteger;
            step_ = Step::kLine;
            break;
          case '$':
            line_kind_ = LineKind::kBulkLen;
            step_ = Step::kLine;
            break;
          case '*':
            line_kind_ = LineKind::kArrayCount;
            step_ = Step::kLine;
            break;
          default:
            return ParseResult::kError;
        }
        break;
      }

      case Step::kLine: {
        const void* nl = std::memchr(data + pos, '\n', len - pos);
        if (nl == nullptr) {
          // 行还没收完：把当前可读字节拷进 line_ 并消费，下次续接不重扫。
          line_.append(data + pos, len - pos);
          pos = len;
          if (line_.size() > kMaxLineLength) return ParseResult::kError;
          *consumed = pos;
          return ParseResult::kIncomplete;
        }
        size_t newline = static_cast<const char*>(nl) - data;
        line_.append(data + pos, newline - pos);
        pos = newline + 1;
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        std::string line = std::move(line_);
        line_.clear();

        switch (line_kind_) {
          case LineKind::kSimple:
          case LineKind::kError: {
            current_ = RespValue{};
            current_.type = (line_kind_ == LineKind::kSimple) ? RespType::kSimpleString
                                                              : RespType::kError;
            current_.str = std::move(line);
            if (Commit(std::move(current_), out)) {
              Reset();
              *consumed = pos;
              return ParseResult::kOk;
            }
            step_ = Step::kType;
            break;
          }
          case LineKind::kInteger: {
            int64_t n = 0;
            if (!ParseInt64(line, &n)) return ParseResult::kError;
            current_ = RespValue{};
            current_.type = RespType::kInteger;
            current_.integer = n;
            if (Commit(std::move(current_), out)) {
              Reset();
              *consumed = pos;
              return ParseResult::kOk;
            }
            step_ = Step::kType;
            break;
          }
          case LineKind::kBulkLen: {
            int64_t n = 0;
            if (!ParseInt64(line, &n)) return ParseResult::kError;
            if (n == -1) {
              if (Commit(RespValue{}, out)) {  // 空 bulk（null）
                Reset();
                *consumed = pos;
                return ParseResult::kOk;
              }
              step_ = Step::kType;
              break;
            }
            if (n < -1 || n > kMaxBulkLength) return ParseResult::kError;
            current_ = RespValue{};
            current_.type = RespType::kBulkString;
            bulk_remaining_ = n;
            step_ = (n == 0) ? Step::kBulkCrlf : Step::kBulkBody;
            break;
          }
          case LineKind::kArrayCount: {
            int64_t n = 0;
            if (!ParseInt64(line, &n)) return ParseResult::kError;
            if (n == -1) {
              if (Commit(RespValue{}, out)) {  // 空 array（null）
                Reset();
                *consumed = pos;
                return ParseResult::kOk;
              }
              step_ = Step::kType;
              break;
            }
            if (n < -1 || n > kMaxArrayElements) return ParseResult::kError;
            if (stack_.size() >= static_cast<size_t>(kMaxNestingDepth)) {
              return ParseResult::kError;
            }
            if (n == 0) {
              RespValue empty;
              empty.type = RespType::kArray;
              if (Commit(std::move(empty), out)) {
                Reset();
                *consumed = pos;
                return ParseResult::kOk;
              }
              step_ = Step::kType;
            } else {
              Frame f;
              f.value.type = RespType::kArray;
              f.remaining = n;
              stack_.push_back(std::move(f));
              step_ = Step::kType;
            }
            break;
          }
        }
        break;
      }

      case Step::kBulkBody: {
        size_t avail = len - pos;
        size_t want = static_cast<size_t>(bulk_remaining_);
        size_t n = (avail < want) ? avail : want;
        current_.str.append(data + pos, n);
        pos += n;
        bulk_remaining_ -= static_cast<int64_t>(n);
        if (bulk_remaining_ > 0) {
          *consumed = pos;
          return ParseResult::kIncomplete;
        }
        step_ = Step::kBulkCrlf;
        break;
      }

      case Step::kBulkCrlf: {
        if (len - pos < 2) {
          // trailer 不足 2 字节：不消费，下次从当前位置重查 \r\n。
          *consumed = pos;
          return ParseResult::kIncomplete;
        }
        if (data[pos] != '\r' || data[pos + 1] != '\n') return ParseResult::kError;
        pos += 2;
        if (Commit(std::move(current_), out)) {
          Reset();
          *consumed = pos;
          return ParseResult::kOk;
        }
        step_ = Step::kType;
        break;
      }
    }
  }
}

}  // namespace mycache
