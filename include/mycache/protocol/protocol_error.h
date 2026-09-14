#pragma once

namespace mycache {

// 协议层面的错误分类。解析器层面的结果由 ParseResult 覆盖；
// 此枚举保留用于更高层的协议违规。
enum class ProtocolError {
  kNone = 0,
  kInvalidMultibulkLength,
  kUnexpectedType,
  kMessageTooLong,
};

inline const char* ProtocolErrorName(ProtocolError e) {
  switch (e) {
    case ProtocolError::kNone: return "no error";
    case ProtocolError::kInvalidMultibulkLength: return "invalid multibulk length";
    case ProtocolError::kUnexpectedType: return "unexpected value type";
    case ProtocolError::kMessageTooLong: return "message too long";
  }
  return "unknown protocol error";
}

}  // namespace mycache
