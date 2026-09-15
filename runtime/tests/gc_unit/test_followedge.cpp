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
#include "Heap/z/zCollectedHeap.hpp"
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

        // The fixture uses a resident
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

// The primitive payload must fit its real multi-unit page (ZPage::size /
// ZPageTable::insert, zPageTable.cpp:44-54). A one-unit descriptor cannot
// represent the large array exercised by this test.
struct LargeArrayFixture {
    LargeArrayFixture()
    {
        const size_t payload = RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD + RegionInfo::UNIT_SIZE;
        const size_t arrayUnits = AlignUp(payload + 128, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
        const size_t units = arrayUnits + 1;
        const size_t metadata = RegionManager::GetMetadataSize(units);
        mappedSize = metadata + units * RegionInfo::UNIT_SIZE;
        mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        GC_EXPECT_TRUE(mapping != MAP_FAILED);
        const MAddress start = reinterpret_cast<MAddress>(mapping) + metadata;
        Heap::OnHeapCreated(start);
        Heap::OnHeapExtended(start + units * RegionInfo::UNIT_SIZE);
        GcHeapFixture::AdvanceGeneration(Generation::Old);
        RegionInfo::Initialize(units, start);
        region0 = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region1 = RegionInfo::InitRegion(1, arrayUnits, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        region1->SetRegionType(RegionInfo::RegionType::RECENT_LARGE_REGION);
        auto* holderType = reinterpret_cast<TypeInfo*>(holderStorage);
        holderType->SetType(TypeKind::TYPE_KIND_CLASS);
        holderType->SetFlagHasRefField();
        holderType->SetInstanceSize(sizeof(void*));
        GCTib tib {};
        tib.tag = SIGN_BIT | 1;
        holderType->SetGCTib(tib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(holderStorage), sizeof(holderStorage));
        obj0 = reinterpret_cast<BaseObject*>(start + 64);
        obj0->SetClassInfo(holderType);
        obj1 = reinterpret_cast<MArray*>(region1->GetRegionStart() + 64);
        obj1->SetClassInfo(GetPrimitiveArrayTypeInfos().array);
        obj1->SetLength(payload);
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + obj0->GetSize());
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + obj1->GetSize());
        GC_EXPECT_TRUE(region1->GetRegionAllocPtr() <= region1->GetRegionEnd());
        GcHeapFixture::AdvanceGeneration(Generation::Old);
    }
    ~LargeArrayFixture()
    {
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region0);
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region1);
        munmap(mapping, mappedSize);
    }
    alignas(TypeInfo) unsigned char holderStorage[sizeof(TypeInfo)] {};
    void* mapping = nullptr;
    size_t mappedSize = 0;
    RegionInfo* region0 = nullptr;
    RegionInfo* region1 = nullptr;
    BaseObject* obj0 = nullptr;
    MArray* obj1 = nullptr;
};

} // namespace

GC_OTHER_VM_TEST(FollowEdge, HolderSlotToLargePrimitiveArrayIsTraced)
{
    LargeArrayFixture fx;
    PrimitiveArrayTypeInfos& infos = GetPrimitiveArrayTypeInfos();

    BaseObject* holder = fx.obj0;
    auto* bytes = reinterpret_cast<MArray*>(fx.obj1);
    bytes->SetClassInfo(infos.array);

    RegionInfo* targetRegion = fx.region1;

    GC_EXPECT_TRUE(bytes->IsPrimitiveArray());
    GC_EXPECT_FALSE(infos.array->HasRefField());
    GC_EXPECT_TRUE(bytes->GetSize() > RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD);

    // Plant holder.bytes. The holder GCTib has bit 0 set, so the exact major
    // non-array walk (WCollector::TraceObjectRefFields) must yield this slot.
    MAddress slotAddress = reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE;
    *reinterpret_cast<MAddress*>(slotAddress) = reinterpret_cast<MAddress>(bytes);

    const auto view = targetRegion->GetMarkView<Generation::Old>();
    size_t holderSlotVisits = 0;
    size_t targetContentVisits = 0;
    size_t pushed = 0;
    auto majorVisitor = [&](RefField<>& field) {
        ++holderSlotVisits;
        BaseObject* target = to_object(field.GetTargetObject());
        GC_EXPECT_TRUE(target == bytes);
        if (!targetRegion->IsMarkedObject(view, target)) {
            ++pushed;
            GC_EXPECT_TRUE(targetRegion->MarkObject(view, target, target->GetSize()));
        }
    };

    MAddress holderContent = reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE;
    holder->GetGCTib().ForEachBitmapWord(holderContent, majorVisitor);

    // Scanning the primitive array itself must visit no element slots. This is
    // independent of enumerating the holder slot above.
    bytes->ForEachRefField([&](RefField<>&) { ++targetContentVisits; });

    GC_EXPECT_EQ(holderSlotVisits, 1u);
    GC_EXPECT_EQ(pushed, 1u);
    GC_EXPECT_TRUE(targetRegion->IsMarkedObject(view, bytes));
    GC_EXPECT_EQ(targetContentVisits, 0u);
}
