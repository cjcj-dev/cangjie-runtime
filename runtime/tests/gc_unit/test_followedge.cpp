// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Regression guard for the major-mark field/content distinction:
// a holder's reference slot must be enumerated even when its referent is a
// primitive RawArray whose own contents correctly contain no references.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Collector/Collector.h"
#include "ObjectModel/MArray.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct PrimitiveArrayTypeInfos {
    PrimitiveArrayTypeInfos()
    {
        std::memset(byteStorage, 0, sizeof(byteStorage));
        byte = reinterpret_cast<TypeInfo*>(byteStorage);
        byte->SetType(TypeKind::TYPE_KIND_UINT8);
        // TypeInfo uses the same size union for instance/component size.
        byte->SetInstanceSize(sizeof(uint8_t));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(byte);

        // The major visitor's PlausibleManagedObjectGate requires a resident
        // TypeInfo. Static storage keeps this registered range alive.
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char byteStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* byte = nullptr;
    TypeInfo* array = nullptr;
};

PrimitiveArrayTypeInfos& GetPrimitiveArrayTypeInfos()
{
    static PrimitiveArrayTypeInfos infos;
    return infos;
}

} // namespace

GC_TEST(FollowEdge, HolderSlotToLargePrimitiveArrayIsTraced)
{
    GcHeapFixture fx;
    PrimitiveArrayTypeInfos& infos = GetPrimitiveArrayTypeInfos();

    BaseObject* holder = fx.obj0;
    auto* bytes = reinterpret_cast<MArray*>(fx.obj1);
    bytes->SetClassInfo(infos.array);
    bytes->SetLength(static_cast<MIndex>(9 * RegionInfo::UNIT_SIZE));

    RegionInfo* targetRegion = fx.region1;
    targetRegion->SetUnitRole(RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    targetRegion->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
    targetRegion->ResetMarkBit();

    GC_EXPECT_TRUE(bytes->IsPrimitiveArray());
    GC_EXPECT_FALSE(infos.array->HasRefField());
    GC_EXPECT_TRUE(bytes->GetSize() > RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD);

    // Plant holder.bytes. The holder GCTib has bit 0 set, so the exact major
    // non-array walk (WCollector::TraceObjectRefFields) must yield this slot.
    MAddress slotAddress = reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE;
    *reinterpret_cast<MAddress*>(slotAddress) = reinterpret_cast<MAddress>(bytes);

    size_t holderSlotVisits = 0;
    size_t targetContentVisits = 0;
    size_t pushed = 0;
    auto majorVisitor = [&](RefField<>& field) {
        ++holderSlotVisits;
        BaseObject* target = to_object(field.GetTargetObject());
        GC_EXPECT_TRUE(target == bytes);
        GC_EXPECT_TRUE(Collector::PlausibleManagedObjectGate("gc_unit.followedge", target));
        if (!targetRegion->IsMarkedObject(target)) {
            ++pushed;
            GC_EXPECT_FALSE(targetRegion->MarkObject(target, target->GetSize()));
        }
    };

    MAddress holderContent = reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE;
    holder->GetGCTib().ForEachBitmapWord(holderContent, majorVisitor);

    // Scanning the primitive array itself must visit no element slots. This is
    // independent of enumerating the holder slot above.
    bytes->ForEachRefField([&](RefField<>&) { ++targetContentVisits; });

    GC_EXPECT_EQ(holderSlotVisits, 1u);
    GC_EXPECT_EQ(pushed, 1u);
    GC_EXPECT_TRUE(targetRegion->IsMarkedObject(bytes));
    GC_EXPECT_EQ(targetContentVisits, 0u);
}
