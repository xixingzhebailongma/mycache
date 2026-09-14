#pragma once

#include <cstddef>

#include "mycache/storage/object/object.h"
#include "mycache/storage/string/sds.h"

namespace mycache {

// 双向链表节点（zmalloc 分配，value 为独立 sds）。
struct ListNode {
  ListNode* prev = nullptr;
  ListNode* next = nullptr;
  sds value = nullptr;
};

// list 对象的内部结构（zmalloc 分配）。Redis 用 quicklist；这里用朴素双向链表，
// 每个节点都走 zmalloc，从而被 used_memory() 精确统计。
struct List {
  ListNode* head = nullptr;
  ListNode* tail = nullptr;
  size_t len = 0;
};

Object* CreateListObject();
List* ListPtr(Object* o);  // 调用方须保证 o 为 kList
void ReleaseListObject(Object* o);  // 释放链表与节点，不释放 Object 结构体本身

// 这些操作负责分配/释放节点；调用方传入的 sds 所有权转移给链表，弹出时归还。
void ListPushFront(List* l, sds value);
void ListPushBack(List* l, sds value);
sds ListPopFront(List* l);
sds ListPopBack(List* l);

}  // namespace mycache
