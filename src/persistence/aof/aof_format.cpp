#include "mycache/persistence/aof/aof_format.h"

#include <array>

#include "mycache/persistence/aof/aof_writer.h"
#include "mycache/protocol/resp_encoder.h"
#include "mycache/protocol/resp_value.h"

namespace mycache {
namespace aof {

uint32_t Crc32(const void* data, size_t len) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();

  uint32_t crc = 0xFFFFFFFFu;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::string EncodeCommand(const std::string& name, const std::vector<std::string>& args) {
  std::vector<RespValue> arr;
  arr.reserve(args.size() + 1);
  arr.push_back(RespValue::Bulk(name));
  for (const auto& a : args) arr.push_back(RespValue::Bulk(a));
  return EncodeResp(RespValue::Array(std::move(arr)));
}

std::string EncodeRecord(const std::string& payload) {
  RecordHeader h;
  h.len = static_cast<uint32_t>(payload.size());
  h.crc = Crc32(payload.data(), payload.size());

  std::string out;
  out.reserve(kMagicSize + payload.size());
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((h.len >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((h.crc >> (8 * i)) & 0xFF));
  out += payload;
  return out;
}

void LogCommand(AofWriter* aof, const std::string& name, const std::vector<std::string>& args) {
  if (aof == nullptr) return;
  aof->Append(EncodeRecord(EncodeCommand(name, args)));
}

}  // namespace aof
}  // namespace mycache
