// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/CollectorResources.h"
#include "Common/ColourPredicates.h"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace { BaseObject* brokenObject = nullptr; }
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
void VerifyAccessedOop(zaddress address)
{
    // Deliberately avoid to_object: this is that conversion's verification leaf.
    ZVerify::Object(reinterpret_cast<BaseObject*>(raw(address)), nullptr);
}
#endif

// zVerify.cpp:119-128. Check real addresses, before reading object metadata.
void ZVerify::Object(BaseObject* object, const void* slot)
{
    const uintptr_t addr = reinterpret_cast<uintptr_t>(object);
    CHECK_DETAIL(addr != 0 && (addr & ~ColourPredicates::HEAP_ADDRESS_MASK) == 0 &&
                 (addr & (alignof(void*) - 1)) == 0 && Heap::IsHeapAddress(addr),
                 "Bad object %p found at %p", object, slot);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
    CHECK_DETAIL(region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion(),
                 "Bad object page %p found at %p", object, slot);
    TypeInfo* type = object->GetTypeInfo();
    CHECK_DETAIL(TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(
                     reinterpret_cast<uintptr_t>(type)) && object->IsValidObject() && type->IsVaildType(),
                 "Bad object type %p found at %p", object, slot);
}

// zVerify.cpp:131-203. Strong old edges and weak-inclusive edges differ in
// both markedness and remembered-set obligations. Always resolve a stale oop
// through the load barrier before checking its real object address.
void ZVerify::Oop(BaseObject* base, RefField<>& field, bool verifyWeaks)
{
    const zpointer value = field.GetFieldValue(std::memory_order_acquire);
    auto& collector = Heap::GetHeap().GetCollector();
    if (!verifyWeaks && value == zpointer::null) {
        // zVerify.cpp:133-136: raw null is only possible when flip promoting.
        CHECK_DETAIL(collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).phase == GC_PHASE_MARK_COMPLETE,
                     "Raw null requires young mark complete at %p", &field);
        RegionInfo* holder = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(base));
        // ZGC retires allocation pages at mark start; Cangjie can keep allocating
        // in a region and represents allocate-black with the holder watermark.
        CHECK_DETAIL(holder->AllocatedAfterMarkStart(
                         reinterpret_cast<MAddress>(base) - holder->GetRegionStart()),
                     "Raw null requires allocating holder at %p", &field);
    }
    if (!ColourPredicates::has_address(raw(value))) { return; }
    RefField<> preloaded(value);
    if (!verifyWeaks && collector.is_mark_good(preloaded)) {
        Object(to_object(preloaded.GetTargetObject()), &field);
        return;
    }
    BaseObject* target = Heap::GetBarrier().ReadReference(base, field);
    Object(target, &field);
    const bool young = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target))->IsYoungRegion();
    if (verifyWeaks) {
        CHECK_DETAIL(ColourPredicates::is_marked_any_old(raw(value), ::g_cjMarkBadMask),
                     "Bad possibly weak oop at %p", &field);
        CHECK_DETAIL(!young || ColourPredicates::is_marked_young(raw(value), ::g_cjMarkBadMask),
                     "Unmarked young oop at %p", &field);
        CHECK_DETAIL(young || RegionSpace::IsMarkedObject<Generation::Old>(target) ||
                     RegionSpace::IsResurrectedObject(target), "Non-live old oop at %p", &field);
        const uintptr_t remset = raw(value) & REMEMBERED_MASK;
        const uintptr_t previous = (::g_cjStoreGoodMask & REMEMBERED_MASK) ^ REMEMBERED_MASK;
        CHECK_DETAIL(remset != previous, "Previous remembered color at %p", &field);
        CHECK_DETAIL(remset == REMEMBERED_MASK ||
                     Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(&field)) ||
                     MutatorManager::Instance().StoreBarrierBufferContains(reinterpret_cast<MAddress>(&field)),
                     "Missing remembered field at %p", &field);
    } else {
        const bool youngMarking = collector.GetCycleSnapshot(GCCycleGeneration::YOUNG).phase == GC_PHASE_TRACE;
        if (!young || !youngMarking) {
            CHECK_DETAIL(ColourPredicates::is_marked_old(raw(value), ::g_cjMarkBadMask),
                         "Unmarked old oop at %p", &field);
            CHECK_DETAIL(!RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(base))->IsYoungRegion(),
                         "Old oop holder must be old at %p", &field);
        }
    }
}

// zVerify.cpp:397-487. Visit the root-reachable graph, including young bridge
// objects, but check outgoing fields only on live old objects.
void ZVerify::Objects(bool verifyWeaks)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked());
    if (Heap::GetHeap().GetCollectorResources().GetYoungDriverPort().Abort().IsRequested()) { return; }
    const auto young = Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG);
    const auto old = Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::OLD);
    DCHECK(young.phase == GC_PHASE_MARK_COMPLETE || old.phase == GC_PHASE_MARK_COMPLETE);
    BaseObject* visitedBase = nullptr;
    const void* visitedSlot = nullptr;
    uintptr_t visitedValue = 0;
    HeapIterator iterator(verifyWeaks, true);
    iterator.Iterate([&](BaseObject* object) {
        Object(object, nullptr);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) { return; }
        if (!RegionSpace::IsMarkedObject<Generation::Old>(object) && !RegionSpace::IsResurrectedObject(object)) {
            LOG(RTLOG_ERROR, "ZVerify found non-live object: %p at %p value=%#zx from=%p",
                object, visitedSlot, visitedValue, visitedBase);
            if (brokenObject == nullptr) { brokenObject = object; }
            return;
        }
        HeapIterator::Fields(object, verifyWeaks, [&](BaseObject* base, RefField<>& field) {
            Oop(base, field, verifyWeaks);
        });
    }, [&](BaseObject* base, const void* slot, uintptr_t value) {
        visitedBase = base;
        visitedSlot = slot;
        visitedValue = value;
    });
    // zVerify.cpp:504 asserts the accumulated old-mark result; the later weak
    // verification logs any non-live reachable objects without this assertion.
    if (!verifyWeaks) { CHECK_DETAIL(brokenObject == nullptr, "Object verification failed"); }
}
} // namespace MapleRuntime
