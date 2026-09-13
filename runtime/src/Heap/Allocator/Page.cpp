// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/PageAllocator.h"

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

#include "Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Collector/Collector.h"
#include "Collector/ZForwarding.h"
#include "Collector/CollectorResources.h"
#include "Collector/CopyCollector.h"
#include "Collector/GcTrigger.h"
#include "Collector/Uncommitter.h"
#include "Base/ZStat.h"
#include "Collector/TenuringThreshold.h"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/CsetEmptyWho.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/FillerZeroDiag.h"
#include "Heap/Verify/HoleWhoDiag.h"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Collector/RelocationSetSelector.h"
#include "Heap/Verify/Zap.h"
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
#if defined(MRT_GC_UNIT_TESTS)
std::atomic<RegionInfo::GhostLookupTestHook> RegionInfo::ghostLookupTestHook { nullptr };
std::atomic<size_t> RegionInfo::ghostLookupTestHookCalls { 0 };


void RegionInfo::SetGhostLookupTestHook(GhostLookupTestHook hook)
{
    ghostLookupTestHookCalls.store(0, std::memory_order_relaxed);
    ghostLookupTestHook.store(hook, std::memory_order_release);
}

size_t RegionInfo::GhostLookupTestHookCalls()
{
    return ghostLookupTestHookCalls.load(std::memory_order_acquire);
}

void RegionInfo::RunGhostLookupTestHook(RegionInfo* region)
{
    GhostLookupTestHook hook = ghostLookupTestHook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook != nullptr) {
        ghostLookupTestHookCalls.fetch_add(1, std::memory_order_relaxed);
        hook(region);
    }
}

#endif
std::atomic<size_t> RegionInfo::markEpochStaleReadCount { 0 };
std::atomic<bool> RegionInfo::markEpochAtexitInstalled { false };
std::atomic<size_t> RegionInfo::ikeTrueEmpty { 0 };
std::atomic<size_t> RegionInfo::ikeConservativeKeep { 0 };
std::atomic<size_t> RegionInfo::ikeConservativeKeepBytes { 0 };
std::atomic<size_t> RegionInfo::ikeNullFaceKeep { 0 };
std::atomic<size_t> RegionInfo::ikeEpochKeep { 0 };
std::atomic<bool> RegionInfo::ikeAtexitInstalled { false };
std::atomic<size_t> RegionInfo::liveCrossMismatchCount { 0 };
std::atomic<size_t> RegionInfo::liveCrossCheckCount { 0 };
std::atomic<bool> RegionInfo::liveCrossAtexitInstalled { false };
std::atomic<size_t> RegionInfo::tipInHeapHits { 0 };

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

void RegionInfo::VisitAllObjects(const std::function<void(BaseObject*)>&& func)
{
    if (IsLargeRegion()) {
        BaseObject* obj = from_region_addr(GetRegionStart());
        // getsize7: dense walk steps via GetSize; reject bad headers instead of SEGV.
        // On reject: stop the walk (cannot invent a step size). Caller sees partial visit.
        if (!Collector::PlausibleManagedObjectGate("VisitAllObjects", obj)) {
            return;
        }
        func(obj);
    } else if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        uintptr_t allocPtr = GetRegionAllocPtr();
        BaseObject* prevObj = nullptr;
        size_t prevSize = 0;
        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // getsize7: GetAllocSize → GetSize reads TypeInfo; interiors/holes SEGV here
            // (deadlock_enqfrontier: VisitLiveObjectsUntilFalse ← RouteRegion ← TryForward).
            // Refuse: break without inventing size — remaining stream is unwalkable.
            if (!Collector::PlausibleManagedObjectGate("VisitAllObjects", obj)) {
                HoleWhoDiag::NoteWalkBreak(this, position, allocPtr, prevObj, prevSize);
                break;
            }
            // GetAllocSize should before call func, because object maybe destroy in compact gc.
            size_t size = RegionSpace::GetAllocSize(*obj);
            func(obj);
            prevObj = obj;
            prevSize = size;
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
    // Skip only when a mark phase established live==0. Bare zero (e.g. non-young under minor)
    // is not an emptiness proof — fall through and consult the mark bitmap.
    if (IsOwnerKnownEmpty()) {
        return true;
    }
    // tipnull arm R: Admit/GetRoute use the typed liveInfo0 face after PrepareForwardable.
    auto survivedAt = [this](size_t offset) -> bool { return IsOwnerSurvivedObject(offset); };
    if (IsLargeRegion()) {
        BaseObject* obj = from_region_addr(GetRegionStart());
        if (!Collector::PlausibleManagedObjectGate("VisitLiveObjects", obj)) {
            return !survivedAt(0);
        }
        return func(obj);
    }
    if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        size_t offset = 0;
        uintptr_t allocPtr = GetRegionAllocPtr();
        size_t regionBytes = allocPtr > GetRegionStart() ? (allocPtr - GetRegionStart()) : 0;

        // tipalign 丙 attempt: cannot skip-and-continue without size (GetAllocSize needs
        // tip; gate tip-misaligned blocks that). Stepping to next liveInfo0 bit lands on
        // multi-bit MarkBits interiors (not object starts) → SEGV. So on gate reject we
        // only refuse to treat the walk as complete if survivors remain (return false).
        // Gate itself is not relaxed.
        auto remainingSurvivor = [&](size_t fromOff) -> bool {
            for (size_t rest = fromOff; rest < regionBytes; rest += kMarkedBytesPerBit) {
                if (survivedAt(rest)) {
                    return true;
                }
            }
            return false;
        };

        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // getsize7: bitten site — PreForward → ForwardObject → RouteRegion → here → GetSize.
            if (!Collector::PlausibleManagedObjectGate("VisitLiveObjects", obj)) {
                // tipwho tip-misaligned at e.g. +6424: do NOT return true (walk success).
                // Incomplete if any liveInfo0 bit remains at/after break (orphan@19400).
                return !remainingSurvivor(offset);
            }
            size_t allocSize = RegionSpace::GetAllocSize(*obj);
            if (allocSize == 0) {
                return !remainingSurvivor(offset);
            }
            position += allocSize;
            if (survivedAt(offset) && !func(obj)) { return false; }
            offset += allocSize;
        }
    }
    return true;
}

#if defined(MRT_GC_UNIT_TESTS)
// mc-r6: keep the unit-test-only mark-cycle entry points in the product
// carrier.  Tests must import these instantiations from libcangjie-runtime.so
// instead of instantiating a second copy in the test executable.
template void RegionInfo::ClearLiveInfo<Generation::Young>(MarkView<Generation::Young>);
template void RegionInfo::ClearLiveInfo<Generation::Old>(MarkView<Generation::Old>);

#endif
} // namespace MapleRuntime

namespace MapleRuntime {
// enroltime: defined out of line so RegionInfo.h does not have to see Heap/GCPhase.
void RegionInfo::NoteEnrolPhase()
{
    // Diagnostic-only. gc_unit fixtures never Heap::Init, so
    // CollectorProxy::currentCollector is null and GetGCPhase would fault.
    // CollectorResources is always constructed; IsGcStarted is false there.
    if (!Heap::GetHeap().GetCollectorResources().IsGcStarted()) {
        return;
    }
    const GCPhase phase = Heap::GetHeap().GetGCPhase();
    const bool afterFlip = (phase == GCPhase::GC_PHASE_PREFORWARD || phase == GCPhase::GC_PHASE_FORWARD);
    std::atomic<uint64_t>& counter = afterFlip ? EnrolAfterFlip() : EnrolBeforeFlip();
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0) {
        return;
    }
    LOG(RTLOG_ERROR, "[ENROLTIME] afterFlip=%d n=%lu phase=%d before=%lu after=%lu", afterFlip ? 1 : 0, n,
        static_cast<int>(phase), EnrolBeforeFlip().load(std::memory_order_relaxed),
        EnrolAfterFlip().load(std::memory_order_relaxed));
}
} // namespace MapleRuntime
