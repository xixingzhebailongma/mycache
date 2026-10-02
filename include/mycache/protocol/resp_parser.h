#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mycache/protocol/resp_value.h"

namespace mycache {

enum class ParseResult { kOk, kIncomplete, kError };

// 有状态增量式 RESP 解析器。Parse() 接收当前可用的字节，并随解析消费输入：半包时
// 不再把字节甩回给调用方重扫，而是记住进度、把已消费的前缀拷贝进内部状态，下次从
// 「未解析处」续接。
//
// 对外契约（调用方必须遵守）：
//  - kOk：*out 为完整顶层值，*consumed 为消耗的字节数；解析器已回到 idle，可继续
//    解析下一条命令（流水线）。
//  - kIncomplete：*consumed 可能 > 0，表示「已安全拷贝进内部状态、可丢弃的前缀」。
//    调用方无论 kOk 还是 kIncomplete，都必须 retrieve(*consumed)，使下次喂入的
//    缓冲正好从未解析处开始。
//  - kError：*consumed == 0；内部状态已脏（未定义），调用方必须 Reset() 或关闭
//    连接，不能继续喂入。
//  - 一次性用法：全新实例 + 完整输入 → kOk 且 *consumed == len。
class RespParser {
 public:
  RespParser() = default;

  // 持有跨调用的解析状态：可移动、不可拷贝。
  RespParser(const RespParser&) = delete;
  RespParser& operator=(const RespParser&) = delete;
  RespParser(RespParser&&) = default;
  RespParser& operator=(RespParser&&) = default;

  ParseResult Parse(const char* data, size_t len, size_t* consumed, RespValue* out);

  // 回到 idle，丢弃任何未完成的解析进度（kError 后复用前调用）。
  void Reset();

 private:
  enum class Step { kType, kLine, kBulkBody, kBulkCrlf };
  enum class LineKind { kArrayCount, kBulkLen, kSimple, kError, kInteger };
  struct Frame {
    RespValue value;    // 正在填充的 array
    int64_t remaining;  // 还差几个元素
  };

  // 完成一个元素并向上提交（含弹出已填满的 array 帧）；返回 true 表示顶层值已写入
  // *out。
  bool Commit(RespValue value, RespValue* out);

  Step step_ = Step::kType;
  LineKind line_kind_ = LineKind::kSimple;
  std::vector<Frame> stack_;   // 嵌套 array 栈；size() 即当前深度
  RespValue current_;          // 正在组装的元素
  int64_t bulk_remaining_ = 0; // bulk 体还差几字节
  std::string line_;           // 跨读拆分的行（已消费、已拷贝的碎片）
};

}  // namespace mycache
