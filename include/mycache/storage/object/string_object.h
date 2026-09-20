#pragma once

#include <cstddef>
#include <cstdint>

#include "mycache/storage/object/object.h"

namespace mycache {

// 创建字符串对象（string 编码，内容存放在独立分配的 sds 中）。
Object* CreateStringObject(const char* buf, size_t len);
// 创建整数对象（int 编码，值直接嵌在 ptr 字段）。
Object* CreateIntObject(long long value);

// 共享整数池大小（对齐 Redis OBJ_SHARED_INTEGERS）。
constexpr int kSharedIntegers = 10000;

// 进程级共享整数对象池：g_shared.integers[v] 是 0..9999 中整数 v 的唯一对象。
// 静态存储默认零初始化；CreateSharedObjects() 之后才被填充。
struct SharedObjects {
  Object* integers[kSharedIntegers];
};
extern SharedObjects g_shared;

// 一次性创建共享整数池（幂等）。应在服务启动时调用。
void CreateSharedObjects();

// 尝试把 string 编码的对象编码为整数（与 Redis 的 tryObjectEncoding 一致）。
// - allow_shared 为 true 且 value ∈ [0, kSharedIntegers) 时，释放原对象并返回共享对象；
// - 否则就地改写为 int 编码并返回原对象；不可编码为整数时原样返回。
Object* TryObjectEncoding(Object* o, bool allow_shared = true);

// 严格地把字符串解析为 long long（等价于 Redis 的 string2ll）：只接受可选
// 前导 '-' 与十进制数字，拒绝前导零、'+'、空白、空串等。
bool StringToLongLong(const char* s, size_t len, long long* out);

}  // namespace mycache
