// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Minimal live-heap fixture for GC unit tests (HotSpot ZTest shape, no GPL code).
// Plants RegionInfo unit table + Heap address range without InitCJRuntime.

#ifndef MRT_GC_HEAP_FIXTURE_HPP
#define MRT_GC_HEAP_FIXTURE_HPP

#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/mman.h>

#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
// Test-only: plant liveInfo/liveInfo0 without product structure change (no 乙).
// RegionInfo::metadata and RegionSpace reserved span are private; unit tests
// need them to Init FDM without Heap::Init / InitCJRuntime.
#define private public
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#undef private
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace GcUnit {

inline zpointer ColouredPointer(BaseObject* object, uintptr_t remap)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    if (address == 0) {
        return zpointer::null;
    }
    const uintptr_t nonRemapFamilies = static_cast<uintptr_t>(::g_cjStoreGoodMask) & ~REMAP_COLOUR_MASK;
    return to_zpointer(address | remap | nonRemapFamilies);
}

inline zpointer StoreGoodPointer(BaseObject* object)
{
    return to_zpointer(MakeStoreGoodSlotWord(reinterpret_cast<uintptr_t>(object),
                                             static_cast<uintptr_t>(::g_cjStoreGoodMask)));
}

struct GcHeapFixture {
    // Six permits the intrusive RegionList port to exercise the same six-node
    // order/removal matrix as OpenJDK test_zList.  Existing fixtures still
    // initialize and use region0/region1 only.
    static constexpr size_t kUnits = 6;

    GcHeapFixture()
    {
        const size_t metadataSize = RegionManager::GetMetadataSize(kUnits);
        mappedSize = metadataSize + kUnits * RegionInfo::UNIT_SIZE;
        mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            std::abort();
        }
        heapStart = reinterpret_cast<MAddress>(mapping) + metadataSize;
        RegionInfo::Initialize(kUnits, heapStart);
        region0 = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region1 = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        Heap::OnHeapCreated(heapStart);
        Heap::OnHeapExtended(heapStart + kUnits * RegionInfo::UNIT_SIZE);
        EnsureForwardData(heapStart);

        std::memset(typeInfoStorage, 0, sizeof(typeInfoStorage));
        typeInfo = reinterpret_cast<TypeInfo*>(typeInfoStorage);
        typeInfo->SetType(TypeKind::TYPE_KIND_CLASS);
        typeInfo->SetFlagHasRefField();
        typeInfo->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        typeInfo->SetGCTib(gctib);
        // Product gate requires TypeInfo residence (TIM image/mmap); stack-planted TI needs note.
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(typeInfoStorage), sizeof(typeInfoStorage));

        obj0 = PlaceObject(heapStart + 64);
        obj1 = PlaceObject(heapStart + RegionInfo::UNIT_SIZE + 64);
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + 64);
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + 64);
    }

    ~GcHeapFixture()
    {
        // SetYoungRegionFlag owns the process-wide youngRegionCount. Fixtures
        // are mapped per test, so leaving their flags set before munmap makes
        // later tests observe young regions that no longer exist.
        if (region0 != nullptr && region0->IsYoungRegion()) {
            region0->SetYoungRegionFlag(0);
        }
        if (region1 != nullptr && region1->IsYoungRegion()) {
            region1->SetYoungRegionFlag(0);
        }
        if (mapping != nullptr && mapping != MAP_FAILED) {
            munmap(mapping, mappedSize);
        }
    }

    BaseObject* PlaceObject(MAddress addr)
    {
        auto* obj = reinterpret_cast<BaseObject*>(addr);
        *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(typeInfo);
        return obj;
    }

    // Product mark faces go through ForwardDataManager. gc_unit never Heap::Init,
    // so initialize the arena before any test asks the product to publish a pair.
    static void EnsureForwardData(MAddress heapStart)
    {
        static bool ready = false;
        if (ready) {
            return;
        }
        auto& space = static_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        if (space.GetMaxCapacity() == 0) {
            constexpr size_t kFdmUnits = 64;
            space.reservedStart = heapStart;
            space.reservedEnd = heapStart + kFdmUnits * RegionInfo::UNIT_SIZE;
        }
        ForwardDataManager::GetForwardDataManager().InitializeForwardData();
        ready = true;
    }

    static RegionBitmap* AllocPlantedBitmap(size_t regionSize)
    {
        size_t bytes = RegionBitmap::GetRegionBitmapSize(regionSize);
        void* mem = std::calloc(1, bytes);
        if (mem == nullptr) {
            std::abort();
        }
        return new (mem) RegionBitmap(regionSize);
    }

    static void FreePlantedBitmap(RegionBitmap*& bitmap)
    {
        if (bitmap == nullptr) {
            return;
        }
        bitmap->~RegionBitmap();
        std::free(bitmap);
        bitmap = nullptr;
    }

    // Hand-plant the product's one page livemap. The template on
    // PlantMarkBitmap remains only to keep typed test call sites concise; G no
    // longer selects storage.
    LiveInfo* PlantLiveInfo(RegionInfo* region)
    {
        auto* live = new LiveInfo();
        live->bindedRegion = region;
        live->GetMarkFace().epoch.store(region->GetSnapshotEpoch(), std::memory_order_relaxed);
        live->GetMarkFace().bitmap = nullptr;
        region->metadata.liveInfo = live;
        return live;
    }

    template<Generation G = Generation::Old>
    RegionBitmap* PlantMarkBitmap(LiveInfo* live, size_t regionSize)
    {
        (void)G;
        if (live->GetMarkFace().bitmap != nullptr) {
            return live->GetMarkFace().bitmap;
        }
        auto* bm = AllocPlantedBitmap(regionSize);
        live->GetMarkFace().bitmap = bm;
        return bm;
    }

    void FreePlanted(LiveInfo* live)
    {
        if (live == nullptr) {
            return;
        }
        RegionBitmap* mark = live->GetMarkFace().bitmap;
        if (mark != nullptr) {
            FreePlantedBitmap(mark);
            live->GetMarkFace().bitmap = nullptr;
        }
        delete live;
    }

    void* mapping = nullptr;
    size_t mappedSize = 0;
    MAddress heapStart = 0;
    RegionInfo* region0 = nullptr;
    RegionInfo* region1 = nullptr;
    BaseObject* obj0 = nullptr;
    BaseObject* obj1 = nullptr;
    alignas(TypeInfo) unsigned char typeInfoStorage[sizeof(TypeInfo)];
    TypeInfo* typeInfo = nullptr;
};

} // namespace GcUnit
} // namespace MapleRuntime

#endif // MRT_GC_HEAP_FIXTURE_HPP
