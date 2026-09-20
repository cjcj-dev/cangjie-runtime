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
#include "Heap/z/zMark.hpp"
#include "ObjectModel/Flags.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zRelocationSet.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "Heap/z/zForwarding.hpp"

namespace MapleRuntime {

inline bool SlotPageRemembered(MapleRuntime::MAddress slot)
{
    MapleRuntime::ZPage* page = MapleRuntime::Heap::page(slot);
    return page != nullptr && page->is_remembered(reinterpret_cast<volatile MapleRuntime::zpointer*>(slot));
}

struct RememberedSet {
    static constexpr size_t kBufferCount = 2;
    struct FlipTouchCounts { size_t bitmap = 0; size_t pageMap = 0; };
    std::unique_ptr<std::atomic<uint64_t>> bitmaps[2];
    std::unique_ptr<std::atomic<uint64_t>> rememberedPages[2];
    std::atomic<int> activeBuffer { 0 };
    bool initialized = true;
    bool IsInitialized() const { return true; }
    void Initialize(MAddress, size_t) { initialized = true; }
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
struct RemsetScanStats {
    size_t live=0;
    size_t consumed=0;
    size_t recorded=0;
    size_t skippedNotHeap=0;
    size_t skippedWeak=0;
};

inline RememberedSet& HeapTestRemset()
{
    static RememberedSet remset;
    return remset;
}


inline bool InitFwdTables()
{
    generation_forwarding_table(Generation::Young) = ZForwardingTable();
    generation_forwarding_table(Generation::Old) = ZForwardingTable();
    return true;
}

inline bool BeginForwardingArena(Generation generation, std::initializer_list<ZPage*> pages)
{
    ZRelocationSetSelector selector;
    for (ZPage* page : pages) {
        if (page == nullptr) {
            continue;
        }
        if (page->IsAllocating()) {
            page->TestMakeRelocatable();
        }
        selector.add_selected_small(page, ZForwarding::nentries(page));
    }
    auto& gen = *(generation == Generation::Young ? static_cast<ZGeneration*>(ZGeneration::young())
                                                 : static_cast<ZGeneration*>(ZGeneration::old()));
    gen.relocation_set().install(&selector);
    // Explicit fixture input for isolated barrier/table cases. This helper is
    // not evidence for the generation entry; SelectionPublishesPreparedForwardingOnce
    // exercises that entry through real allocation and RequestGC.
    ZRelocationSetIterator iterator(&gen.relocation_set());
    for (ZForwarding* forwarding; iterator.next(&forwarding);) {
        forwarding->page()->SetRegionRole(ZPageRole::From);
        gen.forwarding_table().insert(forwarding);
    }
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


struct GcHeapFixture {
    // Six permits the intrusive RegionList port to exercise the same six-node
    // order/removal matrix as OpenJDK test_zList.  Existing fixtures still
    // initialize and use region0/region1 only.
    static void AdvanceGeneration(Generation generation)
    {
        // Construct the Heap before reading the generation singleton pointers.
        (void)Heap::GetHeap();
        auto& cycle = (*(generation == Generation::Young ? static_cast<ZGeneration*>(ZGeneration::young()) : static_cast<ZGeneration*>(ZGeneration::old())));
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

    static constexpr size_t kUnits = 6;

    explicit GcHeapFixture(ZPageType role = ZPageType::small)
    {
        // ZInitialize initializes statistics before any allocation can sample.
        EnsureZAddressDomain();
        ZStat::Initialize();
        (void)Heap::GetHeap();
        RegionManager& manager = Heap::GetHeap().page_allocator();
        region0 = manager.TakeRegion(ZGranuleSize, role, false, false, true);
        region1 = manager.TakeRegion(ZGranuleSize, ZPageType::small, false, false, true);
        CHECK(region0 != nullptr && region1 != nullptr);
        heapStart = region0->GetRegionStart();
        mapping = reinterpret_cast<void*>(heapStart);
        mappedSize = 2 * ZGranuleSize;
        PublishAllocatedPage(region0);
        PublishAllocatedPage(region1);
        for (Generation generation : {Generation::Young, Generation::Old}) {
            if ((*(generation == Generation::Young ? static_cast<ZGeneration*>(ZGeneration::young()) : static_cast<ZGeneration*>(ZGeneration::old()))).Sequence() == 0) {
                AdvanceGeneration(generation);
            }
        }
        // The bitmap fixture uses relocatable pages, as ZLiveMapTest does.
        AdvanceGeneration(Generation::Old);
        AdvanceGeneration(Generation::Young);
        InitFwdTables();

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
        obj1 = PlaceObject(heapStart + ZGranuleSize + 64);
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
        if ((*ZGeneration::young()).Snapshot().active) {
            (*ZGeneration::young()).reset_relocation_set();
            (*ZGeneration::old()).reset_relocation_set();
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
        if (region0 != nullptr) {
            Heap::free_page(region0);
        }
        if (region1 != nullptr) {
            Heap::free_page(region1);
        }
    }

    BaseObject* PlaceObject(MAddress addr)
    {
        auto* obj = reinterpret_cast<BaseObject*>(addr);
        *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(typeInfo);
        return obj;
    }

    // Legacy focused tests used to set ZPage's done word directly.
    // Supply an independent carrier through the product installation API now;
    // this is fixture setup, not evidence of a complete GC entry path.
    void InstallPageOwner(ZPage* region)
    {
        if (region->_scratch.fwdOwner.load(std::memory_order_acquire) != nullptr) return;
        if (generation_forwarding_table(region->GetOwnerGeneration()).get(region->GetRegionStart()) == nullptr) {
            const Generation generation = region->GetOwnerGeneration();
            ZPage* first = region0->GetOwnerGeneration() == generation ? region0 : nullptr;
            ZPage* second = region1->GetOwnerGeneration() == generation ? region1 : nullptr;
            CHECK(BeginForwardingArena(generation, { first, second }));
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

    std::unique_ptr<ZTestAllocatedMemory> heapMapping;
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
