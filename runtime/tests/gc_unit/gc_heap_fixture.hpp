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
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace GcUnit {

struct GcHeapFixture {
    static constexpr size_t kUnits = 2;

    GcHeapFixture()
    {
        mappedSize = kUnits * sizeof(RegionInfo) + kUnits * RegionInfo::UNIT_SIZE;
        mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            std::abort();
        }
        heapStart = reinterpret_cast<MAddress>(mapping) + kUnits * sizeof(RegionInfo);
        RegionInfo::Initialize(kUnits, heapStart);
        region0 = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region1 = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        region1->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        Heap::OnHeapCreated(heapStart);
        Heap::OnHeapExtended(heapStart + kUnits * RegionInfo::UNIT_SIZE);

        std::memset(typeInfoStorage, 0, sizeof(typeInfoStorage));
        typeInfo = reinterpret_cast<TypeInfo*>(typeInfoStorage);
        typeInfo->SetType(TypeKind::TYPE_KIND_CLASS);
        typeInfo->SetFlagHasRefField();
        typeInfo->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        typeInfo->SetGCTib(gctib);

        obj0 = PlaceObject(heapStart + 64);
        obj1 = PlaceObject(heapStart + RegionInfo::UNIT_SIZE + 64);
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + 64);
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + 64);
    }

    ~GcHeapFixture()
    {
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

    // Hand-plant LiveInfo + mark bitmap (no ForwardDataManager / full runtime init).
    LiveInfo* PlantLiveInfo(RegionInfo* region)
    {
        auto* live = new LiveInfo();
        live->bindedRegion = region;
        live->markBitmap = nullptr;
        live->resurrectBitmap = nullptr;
        live->enqueueBitmap = nullptr;
        region->metadata.liveInfo = live;
        return live;
    }

    RegionBitmap* PlantMarkBitmap(LiveInfo* live, size_t regionSize)
    {
        size_t bytes = RegionBitmap::GetRegionBitmapSize(regionSize);
        void* mem = std::calloc(1, bytes);
        if (mem == nullptr) {
            std::abort();
        }
        auto* bm = new (mem) RegionBitmap(regionSize);
        live->markBitmap = bm;
        return bm;
    }

    void FreePlanted(LiveInfo* live)
    {
        if (live == nullptr) {
            return;
        }
        if (live->markBitmap != nullptr) {
            live->markBitmap->~RegionBitmap();
            std::free(live->markBitmap);
            live->markBitmap = nullptr;
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
