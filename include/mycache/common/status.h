#pragma once

#include <string>

#include "mycache/common/error.h"

namespace mycache {

// 轻量级状态类型，携带一个错误码以及可选的消息。
class Status {
 public:
  Status() : code_(ErrorCode::kOk) {}

  static Status Ok() { return Status(); }
  static Status Err(ErrorCode code, std::string msg = "") {
    Status s;
    s.code_ = code;
    s.msg_ = std::move(msg);
    return s;
  }

  bool ok() const { return code_ == ErrorCode::kOk; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    if (ok()) return "OK";
    std::string r = ErrorName(code_);
    if (!msg_.empty()) {
      r += ": ";
      r += msg_;
    }
    return r;
  }

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string msg_;
};

}  // namespace mycache
