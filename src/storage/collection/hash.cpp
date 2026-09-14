#include "mycache/storage/collection/hash.h"

#include <new>

#include "mycache/eviction/lru.h"
#include "mycache/storage/db/dict_types.h"
#include "mycache/storage/dict/dict.h"
#include "mycache/storage/memory/allocator.h"

namespace mycache {

Object* CreateHashObject() {
  Dict* d = static_cast<Dict*>(zmalloc(sizeof(Dict)));
  new (d) Dict(SdsObjectDictType());
  Object* o = static_cast<Object*>(zmalloc(sizeof(Object)));
  o->type = ObjType::kHash;
  o->encoding = ObjEncoding::kHt;
  o->refcount = 1;
  o->ptr = d;
  o->lru = LruClock();
  return o;
}

Dict* HashDict(Object* o) {
  return static_cast<Dict*>(o->ptr);
}

void ReleaseHashObject(Object* o) {
  Dict* d = static_cast<Dict*>(o->ptr);
  if (d) {
    d->~Dict();
    zfree(d);
    o->ptr = nullptr;
  }
}

}  // namespace mycache
