#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mycache {

enum class RespType { kSimpleString, kError, kInteger, kBulkString, kArray, kNull };

// 一个通用的 RESP 值。既用于解析出的命令，也用于回复。
struct RespValue {
  RespType type = RespType::kNull;
  std::string str;         // 物化后有效（回复 / Route 拷贝之后）
  std::string_view view;   // 解析时借引用，指向输入缓冲（data()==nullptr 表示无 view）
  int64_t integer = 0;
  std::vector<RespValue> array;

  RespValue() = default;

  // 拷贝构造：物化 view → str。依赖此副作用：DispatchCommand 的 Route 捕获
  // `args = cmd.array` 靠它把借引用转成 owned string；view 空时退化为普通 str 拷贝。
  RespValue(const RespValue& o)
      : type(o.type),
        str(o.view.data() != nullptr ? std::string(o.view) : o.str),
        integer(o.integer),
        array(o.array) {}

  // move 构造：浅搬 view（保留借引用，不物化）。零拷贝的物理前提——Commit 的 move 链
  // 靠它把 view 从 current_ 搬进 cmd.array 而不产生 string 构造；物化推迟到拷贝构造。
  RespValue(RespValue&& o) noexcept
      : type(o.type),
        str(std::move(o.str)),
        view(o.view),
        integer(o.integer),
        array(std::move(o.array)) {}

  RespValue& operator=(const RespValue& o) {
    if (this != &o) {
      type = o.type;
      str = o.view.data() != nullptr ? std::string(o.view) : o.str;
      view = {};
      integer = o.integer;
      array = o.array;
    }
    return *this;
  }

  RespValue& operator=(RespValue&& o) noexcept {
    if (this != &o) {
      type = o.type;
      str = std::move(o.str);
      view = o.view;
      integer = o.integer;
      array = std::move(o.array);
    }
    return *this;
  }

  // 物化借引用（多 key / 无 key / AOF recovery 在 handler 前显式调用）：
  // view → str 并清空 view，强制「str 与 view 互斥」不变量；递归 array。
  void Materialize() {
    if (view.data() != nullptr) {
      str = std::string(view);
      view = {};
    }
    for (auto& e : array) e.Materialize();
  }

  static RespValue Simple(const std::string& s);
  static RespValue Error(const std::string& s);
  static RespValue Integer(int64_t n);
  static RespValue Bulk(const std::string& s);
  static RespValue NullBulk();
  static RespValue Array(std::vector<RespValue> v);

  bool IsNull() const { return type == RespType::kNull; }
};

}  // namespace mycache
