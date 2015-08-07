// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_IC_INL_H_
#define V8_IC_INL_H_

#include "src/ic/ic.h"

#include "src/compiler.h"
#include "src/debug/debug.h"
#include "src/macro-assembler.h"
#include "src/prototype.h"

namespace v8 {
namespace internal {


Address IC::address() const {
  // Get the address of the call.
  return Assembler::target_address_from_return_address(pc());
}


Address IC::constant_pool() const {
  if (FLAG_enable_embedded_constant_pool) {
    return raw_constant_pool();
  } else {
    return NULL;
  }
}


Address IC::raw_constant_pool() const {
  if (FLAG_enable_embedded_constant_pool) {
    return *constant_pool_address_;
  } else {
    return NULL;
  }
}


Code* IC::GetTargetAtAddress(Address address, Address constant_pool) {
  // Get the target address of the IC.
  Address target = Assembler::target_address_at(address, constant_pool);
  // Convert target address to the code object. Code::GetCodeFromTargetAddress
  // is safe for use during GC where the map might be marked.
  Code* result = Code::GetCodeFromTargetAddress(target);
  DCHECK(result->is_inline_cache_stub());
  return result;
}


void IC::SetTargetAtAddress(Address address, Code* target,
                            Address constant_pool) {
  if (AddressIsDeoptimizedCode(target->GetIsolate(), address)) return;

  DCHECK(target->is_inline_cache_stub() || target->is_compare_ic_stub());

  DCHECK(!target->is_inline_cache_stub() ||
         (target->kind() != Code::LOAD_IC &&
          target->kind() != Code::KEYED_LOAD_IC &&
          (!FLAG_vector_stores || (target->kind() != Code::STORE_IC &&
                                   target->kind() != Code::KEYED_STORE_IC))));

  Heap* heap = target->GetHeap();
  Code* old_target = GetTargetAtAddress(address, constant_pool);
#ifdef DEBUG
  // STORE_IC and KEYED_STORE_IC use Code::extra_ic_state() to mark
  // ICs as language mode. The language mode of the IC must be preserved.
  if (old_target->kind() == Code::STORE_IC ||
      old_target->kind() == Code::KEYED_STORE_IC) {
    DCHECK(StoreICState::GetLanguageMode(old_target->extra_ic_state()) ==
           StoreICState::GetLanguageMode(target->extra_ic_state()));
  }
#endif
  Assembler::set_target_address_at(address, constant_pool,
                                   target->instruction_start());
  if (heap->gc_state() == Heap::MARK_COMPACT) {
    heap->mark_compact_collector()->RecordCodeTargetPatch(address, target);
  } else {
    heap->incremental_marking()->RecordCodeTargetPatch(address, target);
  }
  PostPatching(address, target, old_target);
}


void IC::set_target(Code* code) {
  SetTargetAtAddress(address(), code, constant_pool());
  target_set_ = true;
}


void LoadIC::set_target(Code* code) {
  // The contextual mode must be preserved across IC patching.
  DCHECK(LoadICState::GetTypeofMode(code->extra_ic_state()) ==
         LoadICState::GetTypeofMode(target()->extra_ic_state()));
  // Strongness must be preserved across IC patching.
  DCHECK(LoadICState::GetLanguageMode(code->extra_ic_state()) ==
         LoadICState::GetLanguageMode(target()->extra_ic_state()));

  IC::set_target(code);
}


void StoreIC::set_target(Code* code) {
  // Language mode must be preserved across IC patching.
  DCHECK(StoreICState::GetLanguageMode(code->extra_ic_state()) ==
         StoreICState::GetLanguageMode(target()->extra_ic_state()));
  IC::set_target(code);
}


void KeyedStoreIC::set_target(Code* code) {
  // Language mode must be preserved across IC patching.
  DCHECK(StoreICState::GetLanguageMode(code->extra_ic_state()) ==
         language_mode());
  IC::set_target(code);
}


Code* IC::raw_target() const {
  return GetTargetAtAddress(address(), constant_pool());
}

void IC::UpdateTarget() { target_ = handle(raw_target(), isolate_); }


JSFunction* IC::GetRootConstructor(Map* receiver_map, Context* native_context) {
  Isolate* isolate = receiver_map->GetIsolate();
  if (receiver_map == isolate->heap()->boolean_map()) {
    return native_context->boolean_function();
  } else if (receiver_map->instance_type() == HEAP_NUMBER_TYPE) {
    return native_context->number_function();
  } else if (receiver_map->instance_type() < FIRST_NONSTRING_TYPE) {
    return native_context->string_function();
  } else if (receiver_map->instance_type() == SYMBOL_TYPE) {
    return native_context->symbol_function();
  } else if (receiver_map->instance_type() == FLOAT32X4_TYPE) {
    return native_context->float32x4_function();
  } else if (receiver_map->instance_type() == INT32X4_TYPE) {
    return native_context->int32x4_function();
  } else if (receiver_map->instance_type() == BOOL32X4_TYPE) {
    return native_context->bool32x4_function();
  } else if (receiver_map->instance_type() == INT16X8_TYPE) {
    return native_context->int16x8_function();
  } else if (receiver_map->instance_type() == BOOL16X8_TYPE) {
    return native_context->bool16x8_function();
  } else if (receiver_map->instance_type() == INT8X16_TYPE) {
    return native_context->int8x16_function();
  } else if (receiver_map->instance_type() == BOOL8X16_TYPE) {
    return native_context->bool8x16_function();
  } else {
    return NULL;
  }
}


Handle<Map> IC::GetHandlerCacheHolder(Handle<Map> receiver_map,
                                      bool receiver_is_holder, Isolate* isolate,
                                      CacheHolderFlag* flag) {
  if (receiver_is_holder) {
    *flag = kCacheOnReceiver;
    return receiver_map;
  }
  Context* native_context = *isolate->native_context();
  JSFunction* builtin_ctor = GetRootConstructor(*receiver_map, native_context);
  if (builtin_ctor != NULL) {
    *flag = kCacheOnPrototypeReceiverIsPrimitive;
    return handle(HeapObject::cast(builtin_ctor->instance_prototype())->map());
  }
  *flag = receiver_map->is_dictionary_map()
              ? kCacheOnPrototypeReceiverIsDictionary
              : kCacheOnPrototype;
  // Callers must ensure that the prototype is non-null.
  return handle(JSObject::cast(receiver_map->prototype())->map());
}


Handle<Map> IC::GetICCacheHolder(Handle<Map> map, Isolate* isolate,
                                 CacheHolderFlag* flag) {
  Context* native_context = *isolate->native_context();
  JSFunction* builtin_ctor = GetRootConstructor(*map, native_context);
  if (builtin_ctor != NULL) {
    *flag = kCacheOnPrototype;
    return handle(builtin_ctor->initial_map());
  }
  *flag = kCacheOnReceiver;
  return map;
}


Code* IC::get_host() {
  return isolate()
      ->inner_pointer_to_code_cache()
      ->GetCacheEntry(address())
      ->code;
}


bool IC::AddressIsDeoptimizedCode() const {
  return AddressIsDeoptimizedCode(isolate(), address());
}


bool IC::AddressIsDeoptimizedCode(Isolate* isolate, Address address) {
  Code* host =
      isolate->inner_pointer_to_code_cache()->GetCacheEntry(address)->code;
  return (host->kind() == Code::OPTIMIZED_FUNCTION &&
          host->marked_for_deoptimization());
}
}
}  // namespace v8::internal

#endif  // V8_IC_INL_H_
