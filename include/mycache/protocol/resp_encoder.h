#pragma once

#include <string>

#include "mycache/protocol/resp_value.h"

namespace mycache {

// 将 RespValue 序列化为其 RESP 线上表示。
std::string EncodeResp(const RespValue& v);

}  // namespace mycache
