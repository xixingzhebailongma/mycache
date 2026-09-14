#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mycache {

enum class RespType { kSimpleString, kError, kInteger, kBulkString, kArray, kNull };

// 一个通用的 RESP 值。既用于解析出的命令，也用于回复。
struct RespValue {
  RespType type = RespType::kNull;
  std::string str;
  int64_t integer = 0;
  std::vector<RespValue> array;

  static RespValue Simple(const std::string& s);
  static RespValue Error(const std::string& s);
  static RespValue Integer(int64_t n);
  static RespValue Bulk(const std::string& s);
  static RespValue NullBulk();
  static RespValue Array(std::vector<RespValue> v);

  bool IsNull() const { return type == RespType::kNull; }
};

}  // namespace mycache
