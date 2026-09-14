#pragma once

namespace mycache {

enum class ErrorCode {
  kOk = 0,
  kUnknownCommand,
  kWrongArity,
  kProtocolError,
  kNotInteger,
  kNotBulkString,
  kOutOfMemory,
  kInternal,
};

inline const char* ErrorName(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk: return "OK";
    case ErrorCode::kUnknownCommand: return "unknown command";
    case ErrorCode::kWrongArity: return "wrong number of arguments";
    case ErrorCode::kProtocolError: return "protocol error";
    case ErrorCode::kNotInteger: return "value is not an integer";
    case ErrorCode::kNotBulkString: return "expected bulk string";
    case ErrorCode::kOutOfMemory: return "out of memory";
    case ErrorCode::kInternal: return "internal error";
  }
  return "unknown error";
}

}  // namespace mycache
