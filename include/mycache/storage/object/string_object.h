#pragma once

#include <cstddef>
#include <cstdint>

#include "mycache/storage/object/object.h"

namespace mycache {

// 创建字符串对象（string 编码，内容存放在独立分配的 sds 中）。
Object* CreateStringObject(const char* buf, size_t len);
// 创建整数对象（int 编码，值直接嵌在 ptr 字段）。
Object* CreateIntObject(long long value);

// 尝试把 string 编码的对象编码为整数（与 Redis 的 tryObjectEncoding 一致）。
// 成功则就地改写为 int 编码并返回原对象；否则原样返回。
Object* TryObjectEncoding(Object* o);

// 严格地把字符串解析为 long long（等价于 Redis 的 string2ll）：只接受可选
// 前导 '-' 与十进制数字，拒绝前导零、'+'、空白、空串等。
bool StringToLongLong(const char* s, size_t len, long long* out);

}  // namespace mycache
