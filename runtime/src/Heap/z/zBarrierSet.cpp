// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include "Heap/z/zBarrierSet.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
void ZBarrierSet::on_slowpath_allocation_exit(BaseObject* new_obj)
{
    const ZPage* const page = Heap::page(reinterpret_cast<MAddress>(new_obj));
    if (!page->allows_raw_null()) {
        // ZGC zBarrierSet.cpp:293-301 deoptimizes here. AOT has no deopt;
        // cjcj-llvm#15 only admits allocations that cannot yield after allocation.
        CHECK_DETAIL(false, "allocation exit must allow raw null");
    }
}

// ZGC zBarrierSet.inline.hpp: AccessBarrier::barrier_needed is false for
// primitive value_copy; only reference oop stores/loads need barriers.
bool ZBarrierSet::barrier_needed(bool isReference)
{
    return isReference;
}

void ZBarrierSet::on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native, zaddress_unsafe* root)
{
    data.invisibleRoot = root;
    const auto masks = ThreadGCData::PublishedMasks();
    // Native bootstrap may precede heap/color initialization. A later binding
    // retries attachment before this owner can produce managed references.
    if (masks.storeGood == 0) { return; }
    data.RegisterOwner(owner, native, [&] {
        // ZGC zBarrierSet.cpp:256-267. Publish only after all state is ready.
        data.InstallMasks(masks);
        if (owner != nullptr) {
            owner->GetStackWatermark().Reset();
        }
        data.storeBarrierBuffer->Initialize(masks.storeGood);
    });
}

void ZBarrierSet::on_thread_detach(ThreadGCData& data)
{
    Heap::GetHeap().mark_flush(data);
}

void ZBarrierSet::on_thread_destroy(ThreadGCData& data)
{
    // ZGC zBarrierSet.cpp:248-251. GC data survives detach until SMR deletion.
    data.UnregisterOwner();
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

void ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing(volatile zpointer* p)
{
    ZBarrier::store_barrier_on_heap_oop_field(p, false);
}

void ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_no_keep_alive(volatile zpointer* p)
{
    ZBarrier::no_keep_alive_store_barrier_on_heap_oop_field(p);
}

} // namespace MapleRuntime
