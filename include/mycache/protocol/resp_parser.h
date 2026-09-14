#pragma once

#include <cstddef>

#include "mycache/protocol/resp_value.h"

namespace mycache {

enum class ParseResult { kOk, kIncomplete, kError };

// 增量式 RESP 解析器。Parse() 接收当前可用的字节；要么产出一个完整的顶层值
// （推进 *consumed），要么报告还需要更多数据。无状态，因此调用方可以重复喂入
// 同一段缓冲区。
class RespParser {
 public:
  RespParser() = default;

  ParseResult Parse(const char* data, size_t len, size_t* consumed, RespValue* out);

  static ParseResult ParseOne(const char* data, size_t len, size_t* pos, RespValue* out);
};

}  // namespace mycache
