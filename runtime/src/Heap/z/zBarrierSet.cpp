// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#include "Heap/z/zBarrierSet.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Mutator/Mutator.h"
#include "Common/BaseObject.inline.h"
#include <algorithm>

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

ValuePayload::ValuePayload(MAddress address, size_t size)
    : ValuePayload(address, size, Heap::IsHeapAddress(address) ? Kind::Heap : Kind::Uncolored) {}

ValuePayload::ValuePayload(MAddress address, size_t size, Kind kind)
    : address(address), size(size), kind(kind) {}

ValuePayload::ValuePayload(MAddress address, size_t size, GCTib layout, Kind kind)
    : ValuePayload(address, size, kind)
{
    layout.ForEachBitmapWordInRange(address, [&](RefField<>& slot) {
        offsets.push_back(reinterpret_cast<MAddress>(&slot) - address);
    }, address, address + size);
}

ValuePayload::ValuePayload(MAddress address, size_t size, BaseObject* layout, MAddress layoutStart)
    : ValuePayload(address, size)
{
    if (layout != nullptr) {
        layout->ForEachRefInStruct([&](RefField<>& slot) {
            offsets.push_back(reinterpret_cast<MAddress>(&slot) - layoutStart);
        }, layoutStart, layoutStart + size);
    }
}

ValuePayload::ValuePayload(MAddress address, size_t size, std::vector<size_t> offsets, Kind kind)
    : address(address), size(size), kind(kind), offsets(std::move(offsets))
{
    std::sort(this->offsets.begin(), this->offsets.end());
    this->offsets.erase(std::unique(this->offsets.begin(), this->offsets.end()), this->offsets.end());
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
