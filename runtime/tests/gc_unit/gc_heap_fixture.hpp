#include "gc_cycle_sequence_fixture.hpp"
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
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#undef private
#include "Heap/Collector/LiveInfoArena.h"
#include "Heap/z/zLiveMap.hpp"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "Heap/z/zStat.hpp"

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

// Access the product generation state for the same setup used by ZLiveMapTest.
struct LiveMapCycleAccess : Collector {
    static GenerationCycle& Cycle(Collector& collector, Generation generation)
    {
        // ZLiveMapTest initializes the generation that page/livemap readers use
        // (test_zLiveMap.cpp:45-52). Preserve CollectorProxy's virtual routing.
        return collector.GetGenerationCycle(generation == Generation::Young
            ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
    }
};

struct GcHeapFixture {
    // Six permits the intrusive RegionList port to exercise the same six-node
    // order/removal matrix as OpenJDK test_zList.  Existing fixtures still
    // initialize and use region0/region1 only.
    static void AdvanceGeneration(Generation generation)
    {
        auto& cycle = LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), generation);
        if (cycle.Snapshot().active) {
            cycle.End();
        }
        cycle.Begin(0);
        if (generation == Generation::Young) {
            // Young sequence now advances with the remset flip at mark-start.
            // This liveness-only fixture supplies an empty remembered set;
            // it does not perform a collection of the synthetic heap.
            alignas(8) uint64_t storage[16] {};
            RememberedSet remembered;
            remembered.Initialize(reinterpret_cast<MAddress>(storage), sizeof(storage));
            GenerationSequenceFixture::AdvanceYoung(cycle, remembered);
        } else {
            GenerationSequenceFixture::Advance(cycle);
        }
    }

    // When a fixture replaces the collector, page birth/livemap sequence values
    // must keep the same meaning. Advance the replacement through product starts.
    static void AdoptGenerationIdentity(Collector& next, Collector& previous)
    {
        if (&next == &previous) return;
        for (auto generation : {GCCycleGeneration::YOUNG, GCCycleGeneration::OLD}) {
            auto& cycle = next.GetGenerationCycle(generation);
            const uint64_t sequence = previous.GetCycleSnapshot(generation).sequence;
            while (cycle.Sequence() < sequence) {
                if (cycle.Snapshot().active) cycle.End();
                cycle.Begin(0);
                if (generation == GCCycleGeneration::YOUNG) {
                    alignas(8) uint64_t storage[16] {};
                    RememberedSet remembered;
                    remembered.Initialize(reinterpret_cast<MAddress>(storage), sizeof(storage));
                    GenerationSequenceFixture::AdvanceYoung(cycle, remembered);
                } else {
                    GenerationSequenceFixture::Advance(cycle);
                }
                cycle.End();
            }
        }
    }

    static constexpr size_t kUnits = 6;

    explicit GcHeapFixture(bool withMemoryOwner = false)
    {
        // ZInitialize initializes statistics before any allocation can sample.
        ZStat::Initialize();
        const size_t metadataSize = RegionManager::GetMetadataSize(kUnits);
        mappedSize = metadataSize + kUnits * RegionInfo::UNIT_SIZE;
        if (withMemoryOwner) {
            const MemMap::Option options = { "gc-unit-copy", nullptr,
                MemMap::DEFAULT_MEM_FLAGS, MemMap::DEFAULT_MEM_PROT, false };
            memoryOwner = MemMap::MapMemory(mappedSize, mappedSize, options);
            mapping = memoryOwner == nullptr ? MAP_FAILED : memoryOwner->GetBaseAddr();
        } else {
            mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        if (mapping == MAP_FAILED) {
            std::abort();
        }
        heapStart = reinterpret_cast<MAddress>(mapping) + metadataSize;
        EnsureHeapRange(heapStart);
        // ZHeap::is_in queries the allocated heap ranges, not the address envelope.
        Heap::OnHeapCreated(heapStart, {{heapStart, heapStart + kUnits * RegionInfo::UNIT_SIZE}});
        for (Generation generation : {Generation::Young, Generation::Old}) {
            if (LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), generation).Sequence() == 0) {
                AdvanceGeneration(generation);
            }
        }
        RegionInfo::Initialize(kUnits, heapStart, memoryOwner);
        region0 = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        region1 = RegionInfo::InitRegion(1, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        // The bitmap fixture uses relocatable pages, as ZLiveMapTest does.
        AdvanceGeneration(Generation::Old);
        AdvanceGeneration(Generation::Young);
        ForwardingTable::Initialize(heapStart, kUnits * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);

        std::memset(typeInfoStorage, 0, sizeof(typeInfoStorage));
        typeInfo = reinterpret_cast<TypeInfo*>(typeInfoStorage);
        typeInfo->SetType(TypeKind::TYPE_KIND_CLASS);
        typeInfo->SetFlagHasRefField();
        typeInfo->SetInstanceSize(sizeof(void*));
        GCTib gctib {};
        gctib.tag = SIGN_BIT | 1;
        typeInfo->SetGCTib(gctib);
        // ZVerify checks TypeInfo residence; register this fixture's metadata range.
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(typeInfoStorage), sizeof(typeInfoStorage));

        obj0 = PlaceObject(heapStart + 64);
        obj1 = PlaceObject(heapStart + RegionInfo::UNIT_SIZE + 64);
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + 64);
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + 64);
    }

    ~GcHeapFixture()
    {
        // Destroying this synthetic heap is its explicit remap-coverage
        // boundary. Do not carry forwarding authority into a later fixture
        // whose mmap may reuse the same virtual range.
        // Some life-clock tests intentionally keep several fixtures alive.
        // RegionInfo's unit map is process-global, so only the most recently
        // installed fixture may translate its metadata pointer here.
        if (RegionInfo::UnitInfo::heapStartAddress == heapStart) {
            ForwardingTable::ResetRelocationSet(Generation::Young);
            ForwardingTable::ResetRelocationSet(Generation::Old);
        }
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region0);
        LiveInfoArena::GetLiveInfoArena().RecyclePageLiveInfo(region1);
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
            if (memoryOwner != nullptr) {
                MemMap::DestroyMemMap(memoryOwner);
            } else {
                munmap(mapping, mappedSize);
            }
        }
    }

    BaseObject* PlaceObject(MAddress addr)
    {
        auto* obj = reinterpret_cast<BaseObject*>(addr);
        *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(typeInfo);
        return obj;
    }

    // Keep the synthetic heap's existing 64-unit envelope. Livemap storage is
    // page-owned and no longer depends on a separately initialized fixed arena.
    static void EnsureHeapRange(MAddress heapStart)
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
        ready = true;
    }

    // Legacy focused tests used to set RegionInfo's done word directly.
    // Supply an independent carrier through the product installation API now;
    // this is fixture setup, not evidence of a complete GC entry path.
    void InstallPageOwner(RegionInfo* region)
    {
        if (region->metadata.fwdOwner.load(std::memory_order_acquire) != nullptr) return;
        if (ForwardingTable::GetEntries(region->GetRegionStart(), region->GetOwnerGeneration()) == nullptr) {
            RegionList selected("fixture-forwardings");
            const Generation generation = region->GetOwnerGeneration();
            if (region0->GetOwnerGeneration() == generation) {
                selected.PrependRegion(region0, region0->GetRegionType());
            }
            if (region1->GetOwnerGeneration() == generation) {
                selected.PrependRegion(region1, region1->GetRegionType());
            }
            CHECK(ForwardingTable::BeginForwardingArena(generation, selected));
            while (selected.TakeHeadRegion() != nullptr) {}
        }
        CHECK(ForwardingTable::InstallPublicationBeforeCopy(region->GetRegionStart(), region->GetRegionSize(), region, region->GetOwnerGeneration()));
        CHECK(ForwardingTable::PublishFromPageView(region, region->GetLiveInfo(), region->GetSnapshotEpoch(),
            region->GetRegionAllocPtr(), region->BirthSequence(),
            static_cast<uint8_t>(region->IsYoungRegion() ? Generation::Young : Generation::Old),
            0, region->GetRegionLifeId()));
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
        // Match page ownership: promotion may transfer this livemap.
        auto* live = LiveInfoArena::GetLiveInfoArena().AllocateLiveInfo(region);
        live->bindedRegion = region;
        live->GetMarkFace().epoch.store(region->GetSnapshotEpoch(), std::memory_order_relaxed);
        live->GetMarkFace().bitmap = nullptr;
        live->resurrectBitmap = nullptr;
        live->enqueueBitmap = AllocPlantedBitmap(region->GetRegionSize());
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
        // Remove the same owner registration before destroying its bitmaps.
        auto owned = LiveInfoArena::GetLiveInfoArena().TakePageLiveInfo(live->bindedRegion, live);
    }

    MemMap* memoryOwner = nullptr;
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
