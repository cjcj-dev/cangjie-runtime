// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"

using namespace MapleRuntime;
namespace {
std::atomic<unsigned> failures{0};
std::mutex resultMutex;
void Expect(bool value, const char* name)
{
    std::printf("P2_ASSERT %s %s\n", name, value ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!value) ++failures;
}
TypeInfo* Type(unsigned char* storage, bool refs, unsigned fields)
{
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(fields * sizeof(uintptr_t));
    if (refs) {
        type->SetFlagHasRefField();
        GCTib tib{};
        tib.tag = SIGN_BIT | ((uintptr_t(1) << fields) - 1);
        type->SetGCTib(tib);
    }
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(TypeInfo));
    return type;
}
RefField<>& Slot(BaseObject* object, unsigned index = 0)
{
    return HeapSlotAt<>(reinterpret_cast<MAddress>(object) + sizeof(uintptr_t) * (index + 1));
}
}

// The compiler-generated managed caller owns runtime startup. Inputs use real
// allocation/store/export APIs; no phase, mark, forwarding or remset state is seeded.
extern "C" int p2FieldBarrierExercise()
{
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)]{};
    auto* holderType = Type(types[0], true, 2);
    auto* edgeType = Type(types[1], true, 1);
    auto* leafType = Type(types[2], false, 1);
    auto& heap = Heap::GetHeap();
    auto& collector = heap.GetCollector();
    auto& barrier = Heap::GetBarrier();
    auto* holder = MObject::NewPinnedObject(holderType, 24);
    auto* oldChild = MObject::NewPinnedObject(leafType, 16);
    const U64 root = heap.RegisterExportRoot(holder);
    barrier.WriteReference(holder, Slot(holder), oldChild);
    barrier.WriteReference(holder, Slot(holder, 1), oldChild);
    // Advance a real young epoch before overwriting the old slot. Its previous
    // non-null word must go through the store barrier and remember the slot.
    collector.RequestGC(GC_REASON_YOUNG, false);
    auto* child = MObject::NewObject(edgeType, 16, AllocType::MOVEABLE_OBJECT);
    auto* sentinel = MObject::NewObject(leafType, 16, AllocType::MOVEABLE_OBJECT);
    barrier.WriteReference(child, Slot(child), sentinel);
    barrier.WriteReference(holder, Slot(holder), child);
    Expect(!RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder))->IsYoungRegion(), "real_holder_is_old");
    Expect(RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(child))->IsYoungRegion(), "real_child_is_young");
    unsigned remsetChild = 0, oldOld = 0, oldYoung = 0, youngFollow = 0;
    BaseObject* currentChild = child;
    Barrier::testFieldMarkResult = [&](Barrier::FieldMarkKind kind, RefField<>& field,
                                      zpointer observed, zaddress result) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (&field == &Slot(holder) && kind == Barrier::FieldMarkKind::Remset) {
            ++remsetChild;
            currentChild = to_object(result);
            Expect(currentChild != nullptr, "remset_returns_child");
            if (currentChild != nullptr) {
                auto* page = RegionInfo::GetRegionInfoAt(raw(result));
                Expect(page->IsYoungRegion(), "remset_child_still_young");
                Expect(page->IsMarkedObject(page->GetMarkView<Generation::Young>(), currentChild), "remset_child_marked");
            }
            Expect(ColourPredicates::is_load_good(raw(field.GetFieldValue()), ::g_cjLoadBadMask) &&
                   ColourPredicates::is_marked_young(raw(field.GetFieldValue()), ::g_cjMarkBadMask), "remset_slot_young_good");
        }
        if (&field == &Slot(holder, 1) && kind == Barrier::FieldMarkKind::Old) {
            ++oldOld;
            Expect(to_object(result) == oldChild, "old_old_returns_current");
            auto* page = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(oldChild));
            Expect(page->IsMarkedObject(page->GetMarkView<Generation::Old>(), oldChild), "old_old_marked");
        }
        if (&field == &Slot(holder) && kind == Barrier::FieldMarkKind::Old && currentChild != nullptr &&
            RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(currentChild))->IsYoungRegion()) {
            ++oldYoung;
            Expect(!ColourPredicates::is_mark_good(raw(observed), ::g_cjLoadBadMask, ::g_cjMarkBadMask), "old_young_reached_bad_color");
            Expect(is_null(result), "old_young_no_object_result");
            Expect(field.GetFieldValue() == observed, "old_young_slot_unchanged");
        }
        if (currentChild != nullptr && &field == &Slot(currentChild) && kind == Barrier::FieldMarkKind::Young) {
            ++youngFollow;
            Expect(!is_null(result), "young_child_follows_sentinel");
            Expect(!ColourPredicates::is_store_bad(raw(field.GetFieldValue()), ::g_cjStoreBadMask), "young_field_store_good");
        }
    };
    const bool minorOnly = std::getenv("P2_MINOR_ONLY") != nullptr;
    collector.RequestGC(GC_REASON_YOUNG, false);
    // A second cycle removes the store-buffer current-value marking side path:
    // the unchanged edge must now survive through the remembered slot itself.
    BaseObject* childBeforeNext = barrier.ReadReference(holder, Slot(holder));
    currentChild = childBeforeNext;
    BaseObject* sentinelBeforeNext = barrier.ReadReference(childBeforeNext, Slot(childBeforeNext));
    unsigned completed = 0;
    TracingCollector::testYoungMarkCompleted = [&] {
        ++completed;
        auto* childPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(childBeforeNext));
        auto* sentinelPage = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(sentinelBeforeNext));
        Expect(childPage->IsMarkedObject(childPage->GetMarkView<Generation::Young>(), childBeforeNext),
               "remset_retains_child_next_cycle");
        Expect(sentinelPage->IsMarkedObject(sentinelPage->GetMarkView<Generation::Young>(), sentinelBeforeNext),
               "remset_retains_sentinel_next_cycle");
    };
    collector.RequestGC(GC_REASON_YOUNG, false);
    TracingCollector::testYoungMarkCompleted = nullptr;
    Expect(completed != 0, "young_completion_observation_reached");
    if (!minorOnly) collector.RequestGC(GC_REASON_USER, false);
    Barrier::testFieldMarkResult = nullptr;
    Expect(remsetChild != 0, "real_remset_consumer_reached");
    Expect(youngFollow != 0, "real_young_follow_reached");
    if (!minorOnly) {
        Expect(oldOld != 0, "real_old_old_control_reached");
        Expect(oldYoung != 0, "real_old_young_slow_reached");
    }
    std::printf("P2_RESULT failures=%u remset=%u follow=%u old_old=%u old_young=%u\n",
                failures.load(), remsetChild, youngFollow, oldOld, oldYoung);
    heap.RemoveExportObject(root);
    return failures.load();
}
