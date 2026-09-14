#pragma once

#include "mycache/storage/object/object.h"

namespace mycache {

class Dict;

// 创建空的 hash 对象（type=kHash，encoding=kHt，内部持有 sds->Object* 的 dict）。
Object* CreateHashObject();

// 返回 hash 对象内部持有的 dict（调用方须保证 o 为 kHash/kHt）。
Dict* HashDict(Object* o);

// 释放 hash 对象内部 dict（连同其中所有字段值）；不释放 Object 结构体本身。
void ReleaseHashObject(Object* o);

}  // namespace mycache
