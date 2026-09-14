#include "mycache/storage/collection/list.h"

#include "mycache/eviction/lru.h"
#include "mycache/storage/memory/allocator.h"

namespace mycache {

namespace {

ListNode* NewNode(sds value) {
  // zmalloc 是 malloc，不会执行 ListNode 的默认成员初始化器，须显式置空 prev/next。
  ListNode* n = static_cast<ListNode*>(zmalloc(sizeof(ListNode)));
  n->prev = nullptr;
  n->next = nullptr;
  n->value = value;
  return n;
}

}  // namespace

Object* CreateListObject() {
  List* l = static_cast<List*>(zmalloc(sizeof(List)));
  l->head = nullptr;
  l->tail = nullptr;
  l->len = 0;
  Object* o = static_cast<Object*>(zmalloc(sizeof(Object)));
  o->type = ObjType::kList;
  o->encoding = ObjEncoding::kLinkedList;
  o->refcount = 1;
  o->ptr = l;
  o->lru = LruClock();
  return o;
}

List* ListPtr(Object* o) {
  return static_cast<List*>(o->ptr);
}

void ReleaseListObject(Object* o) {
  List* l = static_cast<List*>(o->ptr);
  if (!l) return;
  ListNode* n = l->head;
  while (n) {
    ListNode* next = n->next;
    if (n->value) sdsfree(n->value);
    zfree(n);
    n = next;
  }
  zfree(l);
  o->ptr = nullptr;
}

void ListPushFront(List* l, sds value) {
  ListNode* n = NewNode(value);
  n->next = l->head;
  if (l->head) l->head->prev = n;
  else l->tail = n;
  l->head = n;
  ++l->len;
}

void ListPushBack(List* l, sds value) {
  ListNode* n = NewNode(value);
  n->prev = l->tail;
  if (l->tail) l->tail->next = n;
  else l->head = n;
  l->tail = n;
  ++l->len;
}

sds ListPopFront(List* l) {
  ListNode* n = l->head;
  if (!n) return nullptr;
  l->head = n->next;
  if (l->head) l->head->prev = nullptr;
  else l->tail = nullptr;
  --l->len;
  sds v = n->value;
  n->value = nullptr;  // 归还给调用方，不在此释放
  zfree(n);
  return v;
}

sds ListPopBack(List* l) {
  ListNode* n = l->tail;
  if (!n) return nullptr;
  l->tail = n->prev;
  if (l->tail) l->tail->next = nullptr;
  else l->head = nullptr;
  --l->len;
  sds v = n->value;
  n->value = nullptr;
  zfree(n);
  return v;
}

}  // namespace mycache
