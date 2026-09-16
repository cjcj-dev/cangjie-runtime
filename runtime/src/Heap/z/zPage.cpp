// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zAddress.inline.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/Collector/CopyCollector.h"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
bool ZPage::OnNamedList(const char* name) const
{
    RegionList* owner = GetRegionListOwner();
    return owner != nullptr && name != nullptr && std::strcmp(owner->GetListName(), name) == 0;
}

// Keep the empty allocation page identity in the product DSO. An inline
// function-local object gives callers in another DSO a different sentinel.
ZPage* ZPage::NullRegion()
{
    static ZPage nullRegion;
    return &nullRegion;
}

size_t ZPage::totalUnitCount = 0;
uintptr_t ZPage::heapStartAddress = 0;
std::vector<ZPage::UnitSegment> ZPage::unitSegments;
ZSafeDelete<ZPage::PageRetirement> ZPage::safeDestroy;

std::atomic<size_t> ZPage::youngRegionCount { 0 };
namespace {
// ZPageAllocator::used_generation (zPageAllocator.cpp:1311). TLAB extents
// vary, so region counts cannot stand in for young-generation byte occupancy.
std::atomic<size_t> youngRegionBytes{ 0 };
}
std::atomic<size_t> ZPage::dispelGhostCount { 0 };

std::mutex ZPage::youngRegionFlagMutex;
void ZPage::SetYoungRegionFlag(uint8_t flag)
{
    std::lock_guard<std::mutex> lock(youngRegionFlagMutex);
    // The bit records charged occupancy, including zero-initialized _scratch.
    // Page identity is published separately below (ZPage::reset, zPage.cpp:103).
    bool wasYoung = _generation_id == ZGenerationId::young;
    bool makeYoung = flag != 0;
    if (!wasYoung && makeYoung) {
        youngRegionBytes.fetch_add(GetRegionSize(), std::memory_order_release);
        youngRegionCount.fetch_add(1, std::memory_order_release);
    }
    _generation_id = makeYoung ? ZGenerationId::young : ZGenerationId::old;
    if (wasYoung && !makeYoung) {
        size_t count = youngRegionCount.load(std::memory_order_relaxed);
        CHECK(count > 0);
        youngRegionCount.fetch_sub(1, std::memory_order_release);
        youngRegionBytes.fetch_sub(GetRegionSize(), std::memory_order_release);
    }
}

size_t ZPage::GetYoungRegionCount()
{
    return youngRegionCount.load(std::memory_order_acquire);
}

size_t RegionManager::GetYoungAllocatedSize() const
{
    return youngRegionBytes.load(std::memory_order_acquire);
}

bool ZPage::HasYoungRegions()
{
    return GetYoungRegionCount() != 0;
}

static size_t GetPageSize() noexcept
{
    size_t pageSize = 0;
#if defined(_WIN64)
    SYSTEM_INFO systeminfo;
    GetSystemInfo(&systeminfo);
    if (systeminfo.dwPageSize != 0) {
        pageSize = systeminfo.dwPageSize;
    } else {
        // default page size is 4KB if get system page size failed.
        pageSize = 4 * KB;
    }
#elif defined(__APPLE__)
    pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    pageSize = static_cast<size_t>(getpagesize());
#endif
    return pageSize;
}

// System default page size
const size_t MRT_PAGE_SIZE = GetPageSize();
const size_t AllocatorUtils::ALLOC_PAGE_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// region unit size: same as system page size
const size_t ZPage::UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// regarding a object as a large object when the size is greater than 32KB or one page size,
// depending on the system page size.
const size_t ZPage::LARGE_OBJECT_DEFAULT_THRESHOLD = MapleRuntime::MRT_PAGE_SIZE > (32 * KB) ?
                                                            MapleRuntime::MRT_PAGE_SIZE : 32 * KB;
// max size of per region is 128KB.
const size_t RegionManager::MAX_UNIT_COUNT_PER_REGION = (128 * KB) / MapleRuntime::MRT_PAGE_SIZE;
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void ZPage::DumpZPage(LogType type) const
{
    DLOG(type, "Region index: %zu, type: %s, address: 0x%zx-0x%zx, allocated(B) %zu, live(B) %zu", GetUnitIdx(),
         GetTypeName(), GetRegionStart(), GetRegionEnd(), GetRegionAllocatedSize(), livemap()->live_bytes());
}

const char* ZPage::GetTypeName() const
{
    static constexpr const char* regionNames[] = {
        "undefined region",
        "thread local region",
        "recent fullregion",
        "from region",
        "unmovable from region",
        "to region",
        "full pinned region",
        "recent pinned region",
        "raw pointer pinned region",
        "tl raw pointer region",
        "large region",
        "recent large region",
        "garbage region",
    };
    auto* owner = GetRegionListOwner();
    return owner != nullptr ? owner->GetListName() : "unlisted";
}
#endif

// ZPage::clone_for_promotion (zPage.cpp:64-71). ZPage is an indexed
// slot rather than a separately allocated page descriptor, so the original
// young page is represented by PromotionPage while the slot becomes old.
std::unique_ptr<ZPage::PromotionPage> ZPage::CloneForPromotion()
{
    CHECK(IsYoungRegion());
    const ZForwarding::FromPageView* from = GetFromPageView();
    ZLiveMap* original = from == nullptr ? livemap() : from->livemap;
    const MAddress originalTop = from == nullptr ? GetRegionAllocPtr() : from->topAtStart;
    // The published from-page livemap is this page's own map (PublishFromPageMetadata
    // passes livemap()); the promotion page takes it over while the slot gets a
    // fresh one (ZGC keeps the whole original ZPage in the relocation set).
    CHECK_DETAIL(original == livemap(), "promotion source livemap does not belong to region %p", this);
    const uint8_t age = GetYoungAge();
    PromoteYoungRegion();
    CHECK(_scratch.retiredLivemap == original);
    _scratch.retiredLivemap = nullptr;
    return std::make_unique<PromotionPage>(std::unique_ptr<ZLiveMap>(original), GetRegionStart(), originalTop, age,
                                           IsLargeRegion());
}

// ZPage::object_iterate (zPage.inline.hpp:320) on the original young page:
// ZLiveMap::iterate (zLiveMap.inline.hpp:141-158) checks the young
// generation's current sequence before reading any bits.
void ZPage::PromotionPage::ObjectIterate(const std::function<void(BaseObject*)>& visitor) const
{
    if (livemap == nullptr) {
        return;
    }
    const int shift = large ? ZObjectAlignmentLargeShift : ZObjectAlignmentSmallShift;
    livemap->iterate(ZGenerationId::young, [&](BitMap::idx_t index) -> bool {
        const MAddress address = start + ((index / 2) << shift);
        if (address < top) {
            visitor(from_region_addr(address));
        }
        return true;
    });
}

// ZPage::verify_live (zPage.cpp:196-203). The forwarding owner holds the
// original page livemap when this metadata facade already describes to-space.
void ZPage::verify_live(uint32_t liveObjects, size_t liveBytes, bool inPlace) const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    CHECK_DETAIL(from != nullptr && from->livemap != nullptr, "Missing forwarding source livemap");
    const ZLiveMap* map = from->livemap;
    if (!inPlace) {
        // In-place relocation has changed the page to allocating
        const ZGenerationId id = static_cast<Generation>(from->owner) == Generation::Young
            ? ZGenerationId::young : ZGenerationId::old;
        MRT_ASSERT(map->is_marked(id), "Should be marked");
        (void)id;
        const GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase(
            static_cast<GCCycleGeneration>(from->owner));
        MRT_ASSERT(phase != GC_PHASE_ENUM && phase != GC_PHASE_TRACE &&
                   phase != GC_PHASE_CLEAR_SATB_BUFFER, "Wrong phase");
        (void)phase;
    }
    CHECK_DETAIL(liveObjects == map->live_objects(), "Invalid number of live objects");
    CHECK_DETAIL(liveBytes == map->live_bytes(), "Invalid number of live bytes");
}

void ZPage::VisitAllObjects(const std::function<void(BaseObject*)>&& func)
{
    if (IsLargeRegion()) {
        BaseObject* obj = from_region_addr(GetRegionStart());
        func(obj);
    } else if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        uintptr_t allocPtr = GetRegionAllocPtr();
        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // GetAllocSize should before call func, because object maybe destroy in compact gc.
            size_t size = RegionSpace::GetAllocSize(*obj);
            func(obj);
            position += size;
        }
    }
}

void ZPage::ClearRelocationResiduals()
{
    // WaitCopiedObjectsUnlocked already ran at Exempt. Do not SetStateCode on
    // LOCKED: a live copier still UnlockObject(FORWARDED) (StateWord.h:183).
    VisitAllObjects([](BaseObject* obj) {
        if (obj != nullptr && obj->IsForwarded()) {
            obj->SetStateCode(ObjectState::NORMAL);
        }
    });
}

} // namespace MapleRuntime



// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/ImmortalWrapper.h"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"

namespace MapleRuntime {
// ZPage::reset_seqnum (zPage.cpp:90-93), after owner selection and before publication.
void ZPage::ResetPageSequence()
{
    const auto owner = IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
    const auto other = IsYoungRegion() ? GCCycleGeneration::OLD : GCCycleGeneration::YOUNG;
    auto& collector = Heap::GetHeap().GetCollector();
    _seqnum = static_cast<uint32_t>(collector.GetCycleSnapshot(owner).sequence);
    _seqnum_other = static_cast<uint32_t>(collector.GetCycleSnapshot(other).sequence);
}

uint64_t ZPage::GetSnapshotEpoch() const
{
    const GCCycleGeneration generation = GetOwnerGeneration() == Generation::Young
        ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
    return Heap::GetHeap().GetCollector().GetCycleSnapshot(generation).sequence;
}
} // namespace MapleRuntime

namespace MapleRuntime {
ZPage::ZPage()
    : _type(ZPageType::small),
      _generation_id(ZGenerationId::old),
      _age(PageAge::old),
      _seqnum(0),
      _seqnum_other(0),
      _partition_id(0),
      _virtual(),
      _top(zoffset_end::invalid),
      _livemap(nullptr),
      _relocate_promoted(false)
    {
        _scratch.allocPtr = reinterpret_cast<uintptr_t>(nullptr);
        _scratch.regionEnd = reinterpret_cast<uintptr_t>(nullptr);
    }

ZPage::ZPage(ZPageType type, PageAge age, const ZVirtualMemory& vmem)
    : _type(type),
      _generation_id(age != PageAge::old ? ZGenerationId::young : ZGenerationId::old),
      _age(age),
      _seqnum(0),
      _seqnum_other(0),
      _partition_id(0),
      _virtual(vmem),
      _top(to_zoffset_end(vmem.start())),
      _livemap(nullptr),
      _relocate_promoted(false)
{
    _scratch.allocPtr = untype(ZOffset::address_unsafe(vmem.start()));
    _scratch.regionEnd = _scratch.allocPtr + vmem.size();
    reset(age);
}

const char* ZPage::type_to_string() const
{
    switch (type()) {
        case ZPageType::small:
            return "Small";
        case ZPageType::medium:
            return "Medium";
        case ZPageType::large:
            return "Large";
        default:
            return "Unknown";
    }
}

ZPage* ZPage::reset(PageAge age)
{
    SetYoungRegionFlag(age != PageAge::old);
    SetYoungAge(age == PageAge::old ? 0 : static_cast<uint8_t>(untype(age)));
    ResetPageSequence();
    return this;
}

ZPage* ZPage::clone_for_promotion() const
{
    CHECK(IsYoungRegion());
    ZPage* page = new ZPage(_type, PageAge::old, _virtual);
    page->_scratch.allocPtr = _scratch.allocPtr;
    page->_scratch.regionEnd = _scratch.regionEnd;
    page->_top = _top;
    ZPageTable::heap_table().replace(const_cast<ZPage*>(this), page);
    return page;
}

uintptr_t ZPage::alloc_object(size_t size)
{
    CHECK(is_allocating());
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    return Alloc(aligned);
}

uintptr_t ZPage::alloc_object_atomic(size_t size)
{
    CHECK(is_allocating());
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    return AtomicAlloc(aligned);
}

bool ZPage::undo_alloc_object(uintptr_t addr, size_t size)
{
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    if (GetRegionAllocPtr() != addr + aligned) {
        return false;
    }
    SetRegionAllocPtr(addr);
    return true;
}

bool ZPage::undo_alloc_object_atomic(uintptr_t addr, size_t size)
{
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    return UndoAllocObjectAtomic(addr, aligned);
}
}
