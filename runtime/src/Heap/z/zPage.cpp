// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"

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
uintptr_t RegionInfo::UnitInfo::totalUnitCount = 0;
uintptr_t RegionInfo::UnitInfo::heapStartAddress = 0;
MemMap* RegionInfo::UnitInfo::memoryOwner = nullptr;
std::vector<RegionInfo::UnitSegment> RegionInfo::unitSegments;
ZGranuleMap<RegionInfo*> RegionInfo::pageOwners;
std::mutex RegionInfo::pageRetirementMutex;
size_t RegionInfo::pageIterationCount = 0;
std::vector<std::function<void()>> RegionInfo::deferredPageRetirements;

std::atomic<size_t> RegionInfo::youngRegionCount { 0 };
namespace {
// ZPageAllocator::used_generation (zPageAllocator.cpp:1311). TLAB extents
// vary, so region counts cannot stand in for young-generation byte occupancy.
std::atomic<size_t> youngRegionBytes{ 0 };
}
std::atomic<size_t> RegionInfo::dispelGhostCount { 0 };

std::atomic<size_t> RegionInfo::ikeTrueEmpty { 0 };
std::atomic<size_t> RegionInfo::ikeConservativeKeep { 0 };
std::atomic<size_t> RegionInfo::ikeConservativeKeepBytes { 0 };
std::atomic<size_t> RegionInfo::ikeNullFaceKeep { 0 };
std::atomic<size_t> RegionInfo::ikeEpochKeep { 0 };
std::atomic<bool> RegionInfo::ikeAtexitInstalled { false };

std::mutex RegionInfo::youngRegionFlagMutex;
void RegionInfo::SetYoungRegionFlag(uint8_t flag)
{
    std::lock_guard<std::mutex> lock(youngRegionFlagMutex);
    bool wasYoung = IsYoungRegion();
    bool makeYoung = flag != 0;
    if (!wasYoung && makeYoung) {
        youngRegionBytes.fetch_add(GetRegionSize(), std::memory_order_release);
        youngRegionCount.fetch_add(1, std::memory_order_release);
    }
    metadata.regionStateBitField.SetAtomicValue(
        RegionStateBitPos::YOUNG_REGION_FLAG, YOUNG_STATE_BIT_LENGTH, makeYoung ? 1 : 0);
    if (wasYoung && !makeYoung) {
        size_t count = youngRegionCount.load(std::memory_order_relaxed);
        CHECK(count > 0);
        youngRegionCount.fetch_sub(1, std::memory_order_release);
        youngRegionBytes.fetch_sub(GetRegionSize(), std::memory_order_release);
    }
}

size_t RegionInfo::GetYoungRegionCount()
{
    return youngRegionCount.load(std::memory_order_acquire);
}

size_t RegionManager::GetYoungAllocatedSize() const
{
    return youngRegionBytes.load(std::memory_order_acquire);
}

bool RegionInfo::HasYoungRegions()
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
const size_t RegionInfo::UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// regarding a object as a large object when the size is greater than 32KB or one page size,
// depending on the system page size.
const size_t RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD = MapleRuntime::MRT_PAGE_SIZE > (32 * KB) ?
                                                            MapleRuntime::MRT_PAGE_SIZE : 32 * KB;
// max size of per region is 128KB.
const size_t RegionManager::MAX_UNIT_COUNT_PER_REGION = (128 * KB) / MapleRuntime::MRT_PAGE_SIZE;
// size of huge page is 2048KB.
const size_t RegionManager::HUGE_PAGE = (2048 * KB) / MapleRuntime::MRT_PAGE_SIZE;;
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionInfo::DumpRegionInfo(LogType type) const
{
    DLOG(type, "Region index: %zu, type: %s, address: 0x%zx-0x%zx, allocated(B) %zu, live(B) %zu", GetUnitIdx(),
         GetTypeName(), GetRegionStart(), GetRegionEnd(), GetRegionAllocatedSize(), GetLiveByteCount());
}

const char* RegionInfo::GetTypeName() const
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
    return regionNames[static_cast<uint8_t>(GetRegionType())];
}
#endif

// ZPage::clone_for_promotion (zPage.cpp:64-71). RegionInfo is an indexed
// slot rather than a separately allocated page descriptor, so the original
// young page is represented by PromotionPage while the slot becomes old.
std::unique_ptr<RegionInfo::PromotionPage> RegionInfo::CloneForPromotion(MarkView<Generation::Young> youngView)
{
    CHECK(youngView.GetRegion() == this && IsYoungRegion());
    const ZForwarding::FromPageView* from = GetFromPageView();
    LiveInfo* original = from == nullptr ? GetLiveInfo() : from->liveInfo;
    const MAddress originalTop = from == nullptr ? GetRegionAllocPtr() : from->topAtStart;
    auto page = std::make_unique<PromotionPage>(
        LiveInfoArena::GetLiveInfoArena().TakePageLiveInfo(this, original),
        GetRegionStart(), originalTop, GetYoungAge(), IsLargeRegion());
    (void)PromoteYoungRegion(youngView);
    return page;
}

// ZPage::object_iterate (zPage.inline.hpp:320): visit the original page's
// ordinary start bits. ZLiveMap::iterate/is_marked (zLiveMap.inline.hpp:41,152)
// checks the original generation's current sequence before reading any bits.
void RegionInfo::PromotionPage::ObjectIterate(const std::function<void(BaseObject*)>& visitor) const
{
    // PromotionPage is always the original young page, even after its RegionInfo
    // slot becomes old. Do not use that slot's owner or freeze a clone-time epoch.
    const uint64_t sequence = Heap::GetHeap().GetCollector().GetCycleSnapshot(GCCycleGeneration::YOUNG).sequence;
    if (liveInfo == nullptr || sequence == 0 ||
        liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) != sequence) {
        return;
    }
    RegionBitmap* bitmap = __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
    if (bitmap == nullptr) {
        return;
    }
    if (large) {
        if (bitmap->IsObjectStart(0)) {
            visitor(from_region_addr(start));
        }
        return;
    }
    for (MAddress address = start; address < top; address += kMarkedBytesPerBit) {
        if (bitmap->IsObjectStart(address - start)) {
            visitor(from_region_addr(address));
        }
    }
}

// ZPage::verify_live (zPage.cpp:196). The forwarding owner holds the
// original page livemap when this metadata facade already describes to-space.
void RegionInfo::VerifyLive(size_t liveObjects, size_t liveBytes, bool inPlace) const
{
    const ZForwarding::FromPageView* from = GetFromPageView();
    CHECK_DETAIL(from != nullptr && from->liveInfo != nullptr, "Missing forwarding source livemap");
    const LiveInfo::MarkFace& face = from->liveInfo->GetMarkFace();
    RegionBitmap* bitmap = __atomic_load_n(&face.bitmap, std::memory_order_relaxed);
    CHECK_DETAIL(bitmap != nullptr, "Missing forwarding source bitmap");
    if (!inPlace) {
        MRT_ASSERT(from->epoch != 0, "Should be marked");
        const GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase(
            static_cast<GCCycleGeneration>(from->owner));
        MRT_ASSERT(phase != GC_PHASE_ENUM && phase != GC_PHASE_TRACE &&
                   phase != GC_PHASE_CLEAR_SATB_BUFFER, "Wrong phase");
    }
    CHECK_DETAIL(liveObjects == bitmap->GetLiveObjects(), "Invalid number of live objects");
    CHECK_DETAIL(liveBytes == bitmap->GetLiveBytes(), "Invalid number of live bytes");
}

void RegionInfo::VisitAllObjects(const std::function<void(BaseObject*)>&& func)
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

void RegionInfo::ClearRelocationResiduals()
{
    // WaitCopiedObjectsUnlocked already ran at Exempt. Do not SetStateCode on
    // LOCKED: a live copier still UnlockObject(FORWARDED) (StateWord.h:183).
    VisitAllObjects([](BaseObject* obj) {
        if (obj != nullptr && obj->IsForwarded()) {
            obj->SetStateCode(ObjectState::NORMAL);
        }
    });
}

bool RegionInfo::VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func)
{
    // ZPage::object_iterate: only object-start bits authorize a header read.
    std::vector<MAddress> objects;
    CollectLiveObjectStarts(objects);
    for (MAddress address : objects) {
        if (!func(from_region_addr(address))) {
            return false;
        }
    }
    return true;
}

// Keep both generation instantiations in the product carrier. Standalone
// tests import the same mark-cycle implementation in every product configuration
// instead of materializing a second inline copy in the test executable.
template void RegionInfo::ClearLiveInfo<Generation::Young>(MarkView<Generation::Young>);
template void RegionInfo::ClearLiveInfo<Generation::Old>(MarkView<Generation::Old>);

} // namespace MapleRuntime



// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/ImmortalWrapper.h"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/LiveInfoArena.h"
#include "Heap/z/zLiveMap.hpp"

namespace MapleRuntime {
uint64_t RegionInfo::GetSnapshotEpoch() const
{
    const GCCycleGeneration generation = GetOwnerGeneration() == Generation::Young
        ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
    return Heap::GetHeap().GetCollector().GetCycleSnapshot(generation).sequence;
}
} // namespace MapleRuntime

namespace MapleRuntime {
RegionInfo::RegionInfo()
    {
        metadata.allocPtr = reinterpret_cast<uintptr_t>(nullptr);
        metadata.regionEnd = reinterpret_cast<uintptr_t>(nullptr);
    }
}
