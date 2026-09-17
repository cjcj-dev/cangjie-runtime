// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Minimal live-heap fixture for GC unit tests (HotSpot ZTest shape, no GPL code).
// Plants ZPage unit table + Heap address range without InitCJRuntime.

#ifndef MRT_GC_HEAP_FIXTURE_HPP
#define MRT_GC_HEAP_FIXTURE_HPP

#include "gc_worker_fixture.hpp"
#include "gc_cycle_sequence_fixture.hpp"
#include <memory>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <atomic>
#include <unordered_set>
#include <vector>

#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
// ZPage::metadata and RegionSpace reserved span are private; unit tests
// need them to Init FDM without Heap::Init / InitCJRuntime.
#define private public
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#undef private
#include "zunittest.hpp"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zRelocationSet.hpp"
#include "Heap/Allocator/RegionList.h"

namespace MapleRuntime {

inline bool SlotPageRemembered(MapleRuntime::MAddress slot)
{
    MapleRuntime::ZPage* page = MapleRuntime::Heap::page(slot);
    return page != nullptr && page->is_remembered(reinterpret_cast<volatile MapleRuntime::zpointer*>(slot));
}

struct RememberedSet {
    std::atomic<int> activeBuffer { 0 };
    bool IsInitialized() const { return true; }
    void Initialize(MAddress, size_t) {}
    bool Contains(MAddress slot) const { return SlotPageRemembered(slot); }
    void Record(MAddress slot)
    {
        ZPage* page = Heap::page(slot);
        if (page != nullptr) {
            page->remember(reinterpret_cast<volatile zpointer*>(slot));
        }
    }
    size_t Size() const { return 0; }
    template<typename C>
    size_t DrainForMinor(C& out)
    {
        (void)out;
        FlipForMinor();
        return 0;
    }
    void FlipForMinor() { ZRememberedSet::flip(); }
    bool ContainsPrevious(MAddress slot) const
    {
        ZPage* page = Heap::page(slot);
        return page != nullptr && page->was_remembered(reinterpret_cast<volatile zpointer*>(slot));
    }
    bool IsClearInRange(MAddress, size_t, bool) const { return true; }
    template<typename... A>
    size_t ScanPreviousForMinor(A&&...) { return 0; }
    void ClearRegion(MAddress, MAddress) {}
    template<typename... A>
    void VisitRememberedPages(A&&...) {}
    template<typename... A>
    size_t TransferObjectSlots(A&&...) { return 0; }
    struct InPlaceSlot {};
    size_t TakeInPlaceSlots(MAddress, MAddress, std::vector<InPlaceSlot>&) { return 0; }
    size_t MoveInPlaceSlots(const std::vector<InPlaceSlot>&, MAddress, MAddress, size_t) { return 0; }
    std::unordered_set<MAddress> Snapshot() const { return {}; }
    size_t ClearBuffer(int) { return 0; }
};


enum class RemsetFilterReceiptReason : uint8_t { kNone=0, kStale=1, kDeadHolder=2, kNoOrigin=3, kBadTarget=4 };
inline void NoteRemsetFilterTestReceipt(MAddress, RemsetFilterReceiptReason, bool) {}
struct RemsetScanStats { size_t live=0; size_t consumed=0; };

inline RememberedSet& HeapTestRemset()
{
    static RememberedSet remset;
    return remset;
}


inline bool InitFwdTables(MAddress start, size_t size, size_t unit)
{
    auto& collector = Heap::GetHeap().GetCollector();
    collector.GetGenerationCycle(Generation::Young).forwarding_table().initialize(size, start, unit);
    collector.GetGenerationCycle(Generation::Old).forwarding_table().initialize(size, start, unit);
    return true;
}

inline bool BeginForwardingArena(Generation generation, RegionList& regions)
{
    Heap::GetHeap().GetCollector().GetGenerationCycle(generation).relocation_set().install_from_regions(regions);
    return true;
}

struct FwdLookup {
    MAddress to{ 0 };
    enum Answer : uint8_t { ArmedHit, ArmedMiss, Unarmed } answer{ Unarmed };
    uint32_t tableId{ 0 };
    uint64_t fromPageEpoch{ 0 };
    RegionLifeId fromPageLifeId{ 0 };
    bool forwardingSnapshotValid{ false };
    MAddress carrierStart{ 0 };
};
inline MAddress UNUSED_InsertMapping(ZForwarding* forwarding, MAddress from, MAddress to)
{
    return forwarding == nullptr ? 0 : forwarding->insert(from, to);
}
inline MAddress UNUSED_InstallMapping(ZForwarding* forwarding, MAddress from, MAddress to)
{
    return UNUSED_InsertMapping(forwarding, from, to);
}
inline void UNUSED_ClearPageOwner(ZPage* region)
{
    if (region != nullptr) {
        region->_scratch.fwdOwner.store(nullptr, std::memory_order_release);
    }
}
inline const ZForwarding::FromPageView* UNUSED_GetFromPageView(ZPage* region)
{
    return region == nullptr ? nullptr : region->GetFromPageView();
}
inline bool UNUSED_InstallPublication(MAddress, size_t, ZPage* region, Generation)
{
    return forwarding_for_page(region) != nullptr;
}
template<typename... Args>
inline bool UNUSED_PublishFromPageView(ZPage* region, Args&&... args)
{
    ZForwarding* forwarding = forwarding_for_page(region);
    if (forwarding == nullptr) {
        return false;
    }
    forwarding->publish_from_page_view(std::forward<Args>(args)...);
    return true;
}
struct UNUSED_Snap {
    struct Carrier {
        uintptr_t tableId{ 0 };
        uint8_t tableGeneration{ 0 };
        MAddress start{ 0 };
    };
    size_t carrierCount{ 0 };
    size_t carrierTotal{ 0 };
    bool carrierOverflow{ false };
    Carrier carriers[4]{};
};
inline UNUSED_Snap UNUSED_Snapshot(MAddress) { return {}; }

inline FwdLookup LookupTo(MAddress from, Generation generation)
{
    ZForwarding* forwarding = generation_forwarding_table(generation).get(from);
    if (forwarding == nullptr) {
        return FwdLookup{ 0, FwdLookup::Unarmed };
    }
    const MAddress to = forwarding->find(from);
    return FwdLookup{ to, to != 0 ? FwdLookup::ArmedHit : FwdLookup::ArmedMiss };
}

namespace GcUnit {

inline zpointer ColouredPointer(BaseObject* object, uintptr_t remap)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(object);
    if (address == 0) {
        return zpointer::null;
    }
    const uintptr_t nonRemapFamilies = static_cast<uintptr_t>(::g_cjStoreGoodMask) & ~ZPointerRemappedMask;
    return ZAddress::color(static_cast<zaddress>(address), remap | nonRemapFamilies);
}

inline zpointer StoreGoodPointer(BaseObject* object)
{
    return to_zpointer(raw(ZAddress::color(static_cast<zaddress>(reinterpret_cast<uintptr_t>(object)), static_cast<uintptr_t>(::g_cjStoreGoodMask))));
}

// Capture a StoreGood word from the current cycle, then run the product
// mark-start flip so that word becomes mark-bad. Weak LoadBarrier only
// enters the blocked/keep-alive slow path when is_mark_good is false
// (zBarrier.cpp:319-324). XOR flips restore on destruction.
struct RestoreMarkFlips {
    bool young = false;
    bool old = false;
    ~RestoreMarkFlips()
    {
        if (old) {
            ZGlobalsPointers::flip_old_mark_start();
        }
        if (young) {
            ZGlobalsPointers::flip_young_mark_start();
        }
    }
};

inline zpointer CaptureStoreGoodThenFlipMark(BaseObject* object, RestoreMarkFlips& restore,
                                             bool flipYoung, bool flipOld)
{
    const zpointer stored = StoreGoodPointer(object);
    if (flipYoung) {
        ZGlobalsPointers::flip_young_mark_start();
        restore.young = true;
    }
    if (flipOld) {
        ZGlobalsPointers::flip_old_mark_start();
        restore.old = true;
    }
    return stored;
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
            GenerationSequenceFixture::AdvanceYoung(cycle);
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
                    GenerationSequenceFixture::AdvanceYoung(cycle);
                } else {
                    GenerationSequenceFixture::Advance(cycle);
                }
                cycle.End();
            }
        }
    }

    static constexpr size_t kUnits = 6;

    explicit GcHeapFixture(ZPageType role = ZPageType::small)
    {
        // ZInitialize initializes statistics before any allocation can sample.
        EnsureZAddressDomain();
        ZStat::Initialize();
        const size_t metadataSize = RegionManager::GetMetadataSize(kUnits);
        mappedSize = metadataSize + kUnits * ZPage::UNIT_SIZE;
        // Reserve in the heap address domain and back it with a committed,
        // mapped backing file, as ZTest's address reserver and backing mocker do.
        heapMapping.reset(new ZTestHeapMapping(mappedSize));
        mapping = heapMapping->base();
        heapStart = reinterpret_cast<MAddress>(mapping) + metadataSize;
        EnsureHeapRange(heapStart);
        // ZHeap::is_in queries the allocated heap ranges, not the address envelope.
        Heap::OnHeapCreated(heapStart, {{heapStart, heapStart + kUnits * ZPage::UNIT_SIZE}});
for (Generation generation : {Generation::Young, Generation::Old}) {
            if (LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), generation).Sequence() == 0) {
                AdvanceGeneration(generation);
            }
        }
        ZPage::Initialize(kUnits, heapStart);
        region0 = ZPage::InitRegion(0, 1, role);
        region1 = ZPage::InitRegion(1, 1, ZPageType::small);
        ZPageTable::heap_table().insert(region0);
        ZPageTable::heap_table().insert(region1);
        // The bitmap fixture uses relocatable pages, as ZLiveMapTest does.
        AdvanceGeneration(Generation::Old);
        AdvanceGeneration(Generation::Young);
        InitFwdTables(heapStart, kUnits * ZPage::UNIT_SIZE, ZPage::UNIT_SIZE);

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
        obj1 = PlaceObject(heapStart + ZPage::UNIT_SIZE + 64);
        region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj0) + 64);
        region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj1) + 64);
    }

    ~GcHeapFixture()
    {
        // Destroying this synthetic heap is its explicit remap-coverage
        // boundary. Do not carry forwarding authority into a later fixture
        // whose mmap may reuse the same virtual range.
        // Some life-clock tests intentionally keep several fixtures alive.
        // ZPage's unit map is process-global, so only the most recently
        // installed fixture may translate its metadata pointer here.
        if (ZPage::heapStartAddress == heapStart &&
            Heap::GetHeap().GetGCPhase(GCCycleGeneration::YOUNG) != GCPhase::GC_PHASE_UNDEF) {
            Heap::GetHeap().GetCollector().GetGenerationCycle(Generation::Young).reset_relocation_set();
            Heap::GetHeap().GetCollector().GetGenerationCycle(Generation::Old).reset_relocation_set();
        }
        // ~ZPage: the page livemaps go with the synthetic heap.
        for (ZPage* region : {region0, region1}) {
            if (region != nullptr) {
                delete region->_scratch.retiredLivemap;
                region->_scratch.retiredLivemap = nullptr;
            }
        }
        // SetYoungRegionFlag owns the process-wide youngRegionCount. Fixtures
        // are mapped per test, so leaving their flags set before munmap makes
        // later tests observe young regions that no longer exist.
        if (region0 != nullptr && region0->IsYoungRegion()) {
            region0->reset(PageAge::old);
        }
        if (region1 != nullptr && region1->IsYoungRegion()) {
            region1->reset(PageAge::old);
        }
        heapMapping.reset();
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
            space.reservedEnd = heapStart + kFdmUnits * ZPage::UNIT_SIZE;
        }
        ready = true;
    }

    // Legacy focused tests used to set ZPage's done word directly.
    // Supply an independent carrier through the product installation API now;
    // this is fixture setup, not evidence of a complete GC entry path.
    void InstallPageOwner(ZPage* region)
    {
        if (region->_scratch.fwdOwner.load(std::memory_order_acquire) != nullptr) return;
        if (generation_forwarding_table(region->GetOwnerGeneration()).get(region->GetRegionStart()) == nullptr) {
            RegionList selected("fixture-forwardings");
            const Generation generation = region->GetOwnerGeneration();
            if (region0->GetOwnerGeneration() == generation) {
                selected.PrependRegion(region0);
            }
            if (region1->GetOwnerGeneration() == generation) {
                selected.PrependRegion(region1);
            }
            CHECK(BeginForwardingArena(generation, selected));
            while (selected.TakeHeadRegion() != nullptr) {}
        }
        ZForwarding* forwarding = forwarding_for_page(region);
        CHECK(forwarding != nullptr);
        forwarding->publish_from_page_view(&region->livemap(), region->GetSnapshotEpoch(),
            region->GetRegionAllocPtr(), region->BirthSequence(),
            static_cast<uint8_t>(region->IsYoungRegion() ? Generation::Young : Generation::Old),
            0, region->GetRegionLifeId());
    }

    // ZPage::mark_object followed by the caller's inc_live (zMark.cpp:405-425):
    // the fixture's stand-in for one marking step on an object of this page.
    static bool MarkStrong(ZPage* region, BaseObject* object)
    {
        bool incLive = false;
        const bool marked = region->mark_object(from_object(object), false, incLive);
        if (incLive) {
            region->inc_live(1, object->GetSize());
        }
        return marked;
    }

    // mark_object(addr, finalizable = true): only the live bit of the pair.
    static bool MarkFinalizable(ZPage* region, BaseObject* object)
    {
        bool incLive = false;
        const bool marked = region->mark_object(from_object(object), true, incLive);
        if (incLive) {
            region->inc_live(1, object->GetSize());
        }
        return marked;
    }

    std::unique_ptr<ZTestHeapMapping> heapMapping;
    void* mapping = nullptr;
    size_t mappedSize = 0;
    MAddress heapStart = 0;
    ZPage* region0 = nullptr;
    ZPage* region1 = nullptr;
    BaseObject* obj0 = nullptr;
    BaseObject* obj1 = nullptr;
    alignas(TypeInfo) unsigned char typeInfoStorage[sizeof(TypeInfo)];
    TypeInfo* typeInfo = nullptr;
};

} // namespace GcUnit
} // namespace MapleRuntime

#endif // MRT_GC_HEAP_FIXTURE_HPP
