#include "mycache/protocol/resp_encoder.h"

namespace mycache {

namespace {
void AppendCRLF(std::string* s) { s->append("\r\n", 2); }
}  // namespace

std::string EncodeResp(const RespValue& v) {
  std::string out;
  switch (v.type) {
    case RespType::kSimpleString:
      out += '+';
      out += v.str;
      AppendCRLF(&out);
      break;
    case RespType::kError:
      out += '-';
      out += v.str;
      AppendCRLF(&out);
      break;
    case RespType::kInteger:
      out += ':';
      out += std::to_string(v.integer);
      AppendCRLF(&out);
      break;
    case RespType::kBulkString:
      out += '$';
      out += std::to_string(v.str.size());
      AppendCRLF(&out);
      out += v.str;
      AppendCRLF(&out);
      break;
    case RespType::kArray:
      out += '*';
      out += std::to_string(v.array.size());
      AppendCRLF(&out);
      for (const auto& e : v.array) out += EncodeResp(e);
      break;
    case RespType::kNull:
    default:
      out += "$-1";
      AppendCRLF(&out);
      break;
  }
  return out;
}

}  // namespace mycache
