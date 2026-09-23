// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include "Heap/z/zBarrierSet.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"

namespace MapleRuntime {
// ZGC zBarrierSet.inline.hpp: AccessBarrier::barrier_needed is false for
// primitive value_copy; only reference oop stores/loads need barriers.
bool ZBarrierSet::barrier_needed(bool isReference)
{
    return isReference;
}

void ZBarrierSet::on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native, zaddress_unsafe* root)
{
    data.Attach(owner, native, root);
}

void ZBarrierSet::on_thread_detach(ThreadGCData& data)
{
    data.Detach();
}

zaddress ZBarrierSet::oop_load_in_heap(volatile zpointer* p)
{
    return ZBarrier::load_barrier_on_oop_field(p);
}

void ZBarrierSet::oop_store_in_heap(volatile zpointer* p, zaddress value)
{
    ZBarrier::store_barrier_on_heap_oop_field(p, false);
    *const_cast<zpointer*>(p) = ZAddress::store_good(value);
}

zaddress ZBarrierSet::oop_xchg_in_heap(volatile zpointer* p, zaddress value)
{
    ZBarrier::store_barrier_on_heap_oop_field(p, true);
    zpointer next = ZAddress::store_good(value);
    zpointer prev = *const_cast<zpointer*>(p);
    *const_cast<zpointer*>(p) = next;
    return ZBarrier::make_load_good(prev);
}

BaseObject* ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded(BaseObject* o, volatile zpointer* p)
{
    return to_object(ZBarrier::load_barrier_on_oop_field_preloaded(p, to_zpointer(reinterpret_cast<uintptr_t>(o))));
}

BaseObject* ZBarrierSetRuntime::load_barrier_on_weak_oop_field_preloaded(BaseObject* o, volatile zpointer* p)
{
    return to_object(ZBarrier::load_barrier_on_weak_oop_field_preloaded(p, to_zpointer(reinterpret_cast<uintptr_t>(o))));
}

BaseObject* ZBarrierSetRuntime::load_barrier_on_phantom_oop_field_preloaded(BaseObject* o, volatile zpointer* p)
{
    return to_object(ZBarrier::load_barrier_on_phantom_oop_field_preloaded(p, to_zpointer(reinterpret_cast<uintptr_t>(o))));
}

void ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing(volatile zpointer* p)
{
    ZBarrier::store_barrier_on_heap_oop_field(p, true);
}

void ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing(volatile zpointer* p)
{
    ZBarrier::store_barrier_on_heap_oop_field(p, false);
}

void ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_no_keep_alive(volatile zpointer* p)
{
    ZBarrier::no_keep_alive_store_barrier_on_heap_oop_field(p);
}

void ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing(volatile zpointer* p)
{
    ZBarrier::store_barrier_on_native_oop_field(p, false);
}

void ZBarrierSetRuntime::load_barrier_on_oop_array(volatile zpointer* p, size_t length)
{
    ZBarrier::load_barrier_on_oop_array(p, length);
}

void* ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr()
{
    return reinterpret_cast<void*>(load_barrier_on_oop_field_preloaded);
}
void* ZBarrierSetRuntime::load_barrier_on_weak_oop_field_preloaded_addr()
{
    return reinterpret_cast<void*>(load_barrier_on_weak_oop_field_preloaded);
}
void* ZBarrierSetRuntime::load_barrier_on_phantom_oop_field_preloaded_addr()
{
    return reinterpret_cast<void*>(load_barrier_on_phantom_oop_field_preloaded);
}
void* ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr()
{
    return reinterpret_cast<void*>(store_barrier_on_oop_field_with_healing);
}
void* ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr()
{
    return reinterpret_cast<void*>(store_barrier_on_oop_field_without_healing);
}
void* ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing_addr()
{
    return reinterpret_cast<void*>(store_barrier_on_native_oop_field_without_healing);
}
void* ZBarrierSetRuntime::load_barrier_on_oop_array_addr()
{
    return reinterpret_cast<void*>(load_barrier_on_oop_array);
}
} // namespace MapleRuntime
