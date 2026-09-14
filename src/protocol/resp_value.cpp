#include "mycache/protocol/resp_value.h"

namespace mycache {

RespValue RespValue::Simple(const std::string& s) {
  RespValue v;
  v.type = RespType::kSimpleString;
  v.str = s;
  return v;
}

RespValue RespValue::Error(const std::string& s) {
  RespValue v;
  v.type = RespType::kError;
  v.str = s;
  return v;
}

RespValue RespValue::Integer(int64_t n) {
  RespValue v;
  v.type = RespType::kInteger;
  v.integer = n;
  return v;
}

RespValue RespValue::Bulk(const std::string& s) {
  RespValue v;
  v.type = RespType::kBulkString;
  v.str = s;
  return v;
}

RespValue RespValue::NullBulk() {
  RespValue v;
  v.type = RespType::kNull;
  return v;
}

RespValue RespValue::Array(std::vector<RespValue> v) {
  RespValue r;
  r.type = RespType::kArray;
  r.array = std::move(v);
  return r;
}

}  // namespace mycache
