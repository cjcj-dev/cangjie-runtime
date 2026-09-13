// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionManager.h"

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
#include "Collector/MutatorAllocRate.h"
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
#include "Heap/WCollector/RelocationSetSelector.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
void RunRemapWindowTestHook(unsigned point, RegionInfo* region, BaseObject* object);
#endif

namespace RecentFullAccounting {
namespace {
std::atomic<size_t> enqueuedRegions{ 0 };
std::atomic<size_t> dequeuedRegions{ 0 };
std::atomic<size_t> currentBytes{ 0 };
std::atomic<size_t> peakBytes{ 0 };
}

void Enqueue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    enqueuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * RegionInfo::UNIT_SIZE;
    const size_t current = currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    size_t peak = peakBytes.load(std::memory_order_relaxed);
    while (peak < current &&
           !peakBytes.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {}
}

void Dequeue(size_t regions, size_t units)
{
    if (regions == 0) {
        return;
    }
    dequeuedRegions.fetch_add(regions, std::memory_order_relaxed);
    const size_t bytes = units * RegionInfo::UNIT_SIZE;
    const size_t before = currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
    CHECK_DETAIL(before >= bytes, "recent-full accounting underflow: before=%zu remove=%zu", before, bytes);
}

void Report(size_t listRegions, size_t listBytes)
{
    const size_t in = enqueuedRegions.load(std::memory_order_relaxed);
    const size_t out = dequeuedRegions.load(std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][recent-full-account] in=%zu out=%zu current_regions=%zu current_bytes=%zu "
         "peak_bytes=%zu list_regions=%zu list_bytes=%zu",
         in, out, in - out, currentBytes.load(std::memory_order_relaxed),
         peakBytes.load(std::memory_order_relaxed), listRegions, listBytes);
}
} // namespace RecentFullAccounting

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
std::atomic<size_t> RegionInfo::oneseqBumpClearYoung { 0 };
std::atomic<size_t> RegionInfo::oneseqBumpClearOld { 0 };
std::atomic<size_t> RegionInfo::oneseqBumpInitRegion { 0 };
std::atomic<size_t> RegionInfo::oneseqBumpResetAfterForward { 0 };
std::atomic<size_t> RegionInfo::oneseqIsKnownEmptyCalls { 0 };
std::atomic<size_t> RegionInfo::oneseqAuthBlocksReclaim { 0 };
std::atomic<size_t> RegionInfo::oneseqAuthAndEmpty { 0 };
std::atomic<size_t> RegionInfo::oneseqAuthNotEmpty { 0 };
std::atomic<size_t> RegionInfo::oneseqNoAuthNotEmpty { 0 };
std::atomic<bool> RegionInfo::oneseqAtexitInstalled { false };
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

void RegionInfo::ReportOneseqCounts(const char* point)
{
    if (!OneseqDiagEnabled()) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][oneseq] point=%s bump_clear_young=%zu bump_clear_old=%zu "
                 "bump_init=%zu bump_reset_fwd=%zu "
                 "ike_calls=%zu auth_blocks=%zu auth_empty=%zu auth_not_empty=%zu noauth_not_empty=%zu "
                 "stale_read=%zu live_cross_check=%zu live_cross_mismatch=%zu\n",
                 point != nullptr ? point : "?",
                 oneseqBumpClearYoung.load(std::memory_order_relaxed),
                 oneseqBumpClearOld.load(std::memory_order_relaxed),
                 oneseqBumpInitRegion.load(std::memory_order_relaxed),
                 oneseqBumpResetAfterForward.load(std::memory_order_relaxed),
                 oneseqIsKnownEmptyCalls.load(std::memory_order_relaxed),
                 oneseqAuthBlocksReclaim.load(std::memory_order_relaxed),
                 oneseqAuthAndEmpty.load(std::memory_order_relaxed),
                 oneseqAuthNotEmpty.load(std::memory_order_relaxed),
                 oneseqNoAuthNotEmpty.load(std::memory_order_relaxed),
                 markEpochStaleReadCount.load(std::memory_order_relaxed),
                 liveCrossCheckCount.load(std::memory_order_relaxed),
                 liveCrossMismatchCount.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

void RegionInfo::EnsureOneseqAtexit()
{
    if (!OneseqDiagEnabled()) {
        return;
    }
    bool expected = false;
    if (oneseqAtexitInstalled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { ReportOneseqCounts("atexit"); });
    }
}
std::mutex RegionInfo::youngRegionFlagMutex;
std::atomic<size_t> g_promotedCrossGenEdgeCount { 0 };

namespace {
BaseObject* ScanFieldHealedTarget(Collector& collector, RefField<>& field)
{
    const ForwardingProvenance provenance{ ForwardingHolderKind::Remset, nullptr, &field };
    return collector.make_load_good(field, provenance);
}
} // namespace

size_t RegionManager::RecordPromotedCrossGenEdges(RegionInfo* region)
{
    if (region == nullptr || !region->IsYoungRegion()) {
        return 0;
    }
    MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
    if (region->IsSafeKnownYoungEmpty(view)) {
        return 0;
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t recorded = 0;
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap(view) != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    bool useLiveOnly = hasObjectLiveness && region->IsLiveCountAuthoritative();
    auto recordFromObject = [region, view, &rememberedSet, &recorded, hasObjectLiveness,
                             useLiveOnly](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        bool survived = hasObjectLiveness &&
            region->IsSurvivedObject(view, region->GetAddressOffset(reinterpret_cast<MAddress>(object)));
        if (useLiveOnly && !survived) {
            return;
        }
        object->ForEachRefField([&rememberedSet, &recorded, object](RefField<>& field) {
            BaseObject* target = ScanFieldHealedTarget(Heap::GetHeap().GetCollector(), field);
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                rememberedSet.Record(slot);
                ++recorded;
                PromotedRegionDomain::NoteOldProductRecord(slot);
            }
        });
    };
    region->VisitAllObjects([&recordFromObject](BaseObject* object) { recordFromObject(object); });
    if (recorded != 0) {
        g_promotedCrossGenEdgeCount.fetch_add(recorded, std::memory_order_relaxed);
    }

    return recorded;
}

size_t RegionManager::ConsumePromotedCrossGenEdgeCount()
{
    return g_promotedCrossGenEdgeCount.exchange(0, std::memory_order_relaxed);
}

size_t RegionManager::RecordPinnedCrossGenEdges()
{
    MRT_PHASE_TIMER("young.pinned_scan");
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    std::atomic<size_t> recorded{ 0 };
    auto skipPinnedScanRegion = [](RegionInfo* region) {
        // Drain/rescan cost on sd256 was 59.8% of young.* because this walk stamped
        // from-space / free / ghost slots; RescanRememberedSet then dropped them as
        // deadHolder (fysgone: consumed/recorded = 3.3%). ZGC does not walk from-space
        // into the remset (zRemembered.cpp:347-387 found-old; scan is previous face only).
        return region == nullptr || region->IsYoungRegion() || region->IsGarbageRegion() ||
            region->IsFreeRegion() || region->IsFromRegion() || region->IsGhostFromRegion() ||
            region->IsUnmovableFromRegion();
    };
    auto scanRegion = [&rememberedSet, &recorded, &skipPinnedScanRegion](RegionInfo* region) {
        if (skipPinnedScanRegion(region)) {
            return;
        }
        region->VisitAllObjects([&rememberedSet, &recorded, region](BaseObject* object) {
            if (object == nullptr || !object->HasRefField()) {
                return;
            }
            object->ForEachRefField([&rememberedSet, &recorded, region](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                const MAddress targetAddr = reinterpret_cast<MAddress>(target);
                RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(targetAddr);
                if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                    // ZGC sets a remembered bit only for a value its barrier has just
                    // resolved to a live young address: ZRemembered::scan_field calls
                    // remember(p) on the result of remset_barrier_on_oop_field
                    // (zRemembered.cpp:578-589), and remap_and_maybe_add_remset calls
                    // ZRelocate::add_remset only after load_barrier_on_oop_field_preloaded
                    // (zRelocate.cpp:1240-1255).  This walk has no such proof.  Qualifying
                    // the *holder* is not available either -- measured, NW256/256MB: all
                    // 1875-2761 regions it scans carry no liveness face, 0 of 3.68M objects
                    // answer survived, so a holder filter here would delete 100% of the
                    // 551,449 bits it produces rather than filter them.
                    //
                    // The *value* is qualifiable without any liveness face.  A young page's
                    // allocated range is [start, allocPtr); this walk runs with every
                    // mutator stopped (Generation.cpp:627,745) and allocation bumps that
                    // same pointer (RegionInfo.h:3320,3330), so an address at or beyond it
                    // designates no object in the page's current life.  Recording a bit for
                    // one produces an edge no consumer can honour: the rescan hands it to
                    // ResolveStoreValue, which fail-closes on the zero header.  Measured
                    // 3/3 at page+0xbf80 with allocOff=10560, and before that at
                    // page+0xd100 with allocOff=0, both from 48-byte holders with
                    // survived=0 marked=0.
                    if (targetAddr >= targetRegion->GetRegionAllocPtr()) {
                        return;
                    }
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    rememberedSet.Record(slot);
                    recorded.fetch_add(1, std::memory_order_relaxed);

                }
            });
        });
    };
    // zCollectedHeap.cpp:310-311: public safepoint work uses the heap's
    // runtime workers, independently of either generation's GC workers.
    RuntimeWorkers& workers = Heap::GetHeap().GetCollectorResources().GetRuntimeWorkers();
    // Keep page descriptors stable for the entire worker gang, as for the
    // serial page-table iterator. No page-pointer snapshot is needed.
    RegionInfo::PageIterationScope iteration;
    class PinnedScanTask : public GCWorkerTask {
    public:
        explicit PinnedScanTask(const std::function<void(RegionInfo*)>& scan)
            : iterator(RegionInfo::pageOwners), scan(scan) {}

        void Work(uint32_t) override
        {
            iterator.do_pages([&](RegionInfo* region) {
                // Preserve the former pinned/large/full list domain. The trace
                // caches use RECENT_LARGE_REGION and RECENT_FULL_REGION too.
                switch (region->GetRegionType()) {
                    case RegionInfo::RegionType::RECENT_PINNED_REGION:
                    case RegionInfo::RegionType::FULL_PINNED_REGION:
                    case RegionInfo::RegionType::RAW_POINTER_PINNED_REGION:
                    case RegionInfo::RegionType::RECENT_LARGE_REGION:
                    case RegionInfo::RegionType::LARGE_REGION:
                    case RegionInfo::RegionType::RECENT_FULL_REGION:
                        scan(region);
                        break;
                    default:
                        break;
                }
                return true;
            });
        }

    private:
        ZPageTableParallelIterator<RegionInfo*> iterator;
        const std::function<void(RegionInfo*)> scan;
    } task(scanRegion);
    workers.Run(task);
    return recorded.load(std::memory_order_relaxed);
}

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

#if defined(MRT_TESTABLE_INTERNALS)
template<Generation G>
void ForwardTask<G>::Work(uint32_t)
{
    detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
}
#endif

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

void RegionList::MergeRegionList(RegionList& srcList, RegionInfo::RegionType regionType)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    RegionInfo* head = regionList.GetHeadRegion();
    RegionInfo* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    regionList.SetElementType(regionType);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
    for (RegionInfo* node = head; node != nullptr; node = node->GetNextRegion()) {
        node->SetRegionListOwner(this);
    }
}

void RegionList::PrependRegion(RegionInfo* region, RegionInfo::RegionType type)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region, type);
}

void RegionList::PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    if (region == nullptr) {
        return;
    }

    CHECK_DETAIL(region->GetRegionListOwner() == nullptr, "region already belongs to a list");

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
    region->SetRegionListOwner(this);
    region->SetPrevRegion(nullptr);
    IncCounts(1, region->GetUnitCount());
    region->SetNextRegion(listHead);
    if (listHead == nullptr) {
        MRT_ASSERT(listTail == nullptr, "PrependRegion listTail is not null");
        listTail = region;
    } else {
        listHead->SetPrevRegion(region);
    }
    listHead = region;
}

void RegionList::DeleteRegionLocked(RegionInfo* del)
{
    MRT_ASSERT(listHead != nullptr && listTail != nullptr, "illegal region list");
    CHECK_DETAIL(del != nullptr && del->GetRegionListOwner() == this, "region belongs to another list");

    RegionInfo* pre = del->GetPrevRegion();
    RegionInfo* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);
    del->SetRegionListOwner(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), del->GetRegionType());

    DecCounts(1, del->GetUnitCount());

    if (listHead == del) { // delete head
        MRT_ASSERT(pre == nullptr, "Delete Region pre is not null");
        listHead = next;
        if (listHead == nullptr) { // now empty
            listTail = nullptr;
            return;
        }
    } else if (pre != nullptr) {
        pre->SetNextRegion(next);
    }

    if (listTail == del) { // delete tail
        MRT_ASSERT(next == nullptr, "Delete Region next is not null");
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else if (next != nullptr) {
        next->SetPrevRegion(pre);
    } else if (pre != nullptr) {
        // next was stolen (region re-homed onto another list) while this list
        // still named it. Treat del as the last node we still own.
        listTail = pre;
    }
}

#ifdef MRT_DEBUG
void RegionList::DumpRegionList(const char* msg)
{
    DLOG(REGION, "dump region list %s", msg);
    std::lock_guard<std::mutex> lock(listMutex);
    for (RegionInfo *region = listHead; region != nullptr; region = region->GetNextRegion()) {
        DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) units [%zu+%zu, %zu) type %u prev %p next %p", region,
            region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
            region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
            region->GetRegionType(), region->GetPrevRegion(), region->GetNextRegion());
    }
}
#endif
inline void RegionManager::TagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_HUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

inline void RegionManager::UntagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_NOHUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

size_t FreeRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    size_t dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    if (dirtyBytes <= targetCachedSize) {
        VLOG(REPORT, "release heap garbage memory 0 bytes, cache %zu(%zu) bytes", dirtyBytes, targetCachedSize);
        return 0;
    }

    size_t releasedBytes = 0;
    while (dirtyBytes > targetCachedSize) {
        std::lock_guard<std::mutex> lock1(dirtyUnitTreeMutex);
        auto node = dirtyUnitTree.RootNode();
        if (node == nullptr) { break; }
        Index idx = node->GetIndex();
        UnitCount num = node->GetCount();
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(idx));
        dirtyUnitTree.ReleaseRootNode();
        std::lock_guard<std::mutex> lock2(releasedUnitTreeMutex);
        CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true), "tid %d: failed to release garbage units[%u+%u, %u)",
                     GetTid(), idx, num, idx + num);
        releasedBytes += (num * RegionInfo::UNIT_SIZE);
        dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    }
    VLOG(REPORT, "release heap garbage memory %zu bytes, cache %zu(%zu) bytes",
         releasedBytes, dirtyBytes, targetCachedSize);
    return releasedBytes;
}

bool FreeRegionManager::TakeUncommitMemory(size_t maxBytes, uint64_t idleBeforeNs, PageMemory& memory)
{
    if (maxBytes < RegionInfo::UNIT_SIZE) {
        return false;
    }
    UnitIndex idx = 0;
    UnitCount num = 0;
    std::lock_guard<std::mutex> cacheLock(releasedUnitTreeMutex);
    const UnitCount limit = static_cast<UnitCount>(maxBytes / RegionInfo::UNIT_SIZE);
    if (!releasedUnitTree.TakeIdleUnits(idleBeforeNs, limit, idx, num)) {
        return false;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(idx));
    bool inRelocate = false;
    if (Heap::GetHeap().IsGcStarted()) {
        const GCPhase phase = Heap::GetHeap().GetGCPhase();
        inRelocate = phase == GCPhase::GC_PHASE_POST_TRACE ||
                     phase == GCPhase::GC_PHASE_PREFORWARD ||
                     phase == GCPhase::GC_PHASE_FORWARD;
    }
    if (inRelocate || !ExtentReadyForReleasedCache(region)) {
        CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true),
                     "tid %d: failed to restore uncommit units under live forwarding [%u+%u, %u)",
                     GetTid(), idx, num, idx + num);
        return false;
    }
    memory = PageMemory{idx, num, 0, false};
    return true;
}

void FreeRegionManager::ReturnUncommitMemory(const PageMemory& memory)
{
    std::lock_guard<std::mutex> cacheLock(releasedUnitTreeMutex);
    CHECK_DETAIL(releasedUnitTree.MergeInsert(memory.index, memory.units, true),
                 "tid %d: failed to retain uncommit units[%zu+%zu)", GetTid(),
                 static_cast<size_t>(memory.index), static_cast<size_t>(memory.units));
}

void RegionManager::SetMaxUnitCountForRegion(size_t regionSize)
{
    maxUnitCountPerRegion = regionSize * KB / RegionInfo::UNIT_SIZE;
}

void RegionManager::SetMaxUnitCountForPinnedRegion(size_t regionSize)
{
    auto env = std::getenv("cjPinnedRegionSize");
    if (env == nullptr) {
        maxUnitCountPerPinnedRegion = maxUnitCountPerRegion;
        return;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    if (size >= minSize && size <= regionSize) {
        maxUnitCountPerPinnedRegion = size * KB / RegionInfo::UNIT_SIZE;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjPinnedRegionSize parameter. Valid cjPinnedRegionSize"
            "range is [%zuKB, %zuKB].\n", minSize, regionSize);
    }
}

void RegionManager::SetLargeObjectThreshold(size_t configuredRegionSize)
{
    auto env = std::getenv("cjLargeThresholdSize");
    if (env == nullptr) {
        // default value is 32 KB
        largeObjectThreshold = 32 * KB;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    // 64UL: The maximum region size, measured in KB, the value is 2048 KB.
    size_t maxSize = 10 * 1024UL;
    if (size >= minSize && size <= maxSize) {
        largeObjectThreshold = size * KB;
    } else if (size != 0) {
        LOG(RTLOG_ERROR, "Unsupported cjLargeThresholdSize parameter. Valid cjLargeThresholdSize"
            "range is [%zuKB, 2048KB].\n", minSize);
    }
    size_t regionSize = configuredRegionSize * KB;
    largeObjectThreshold = largeObjectThreshold > regionSize ? regionSize :  largeObjectThreshold;
}

void RegionManager::SetGarbageThreshold(double garbageThreshold)
{
    fromSpaceGarbageThreshold = garbageThreshold;
}

#if defined(__EULER__)
void RegionManager::SetCacheRatio(double minSize, double maxSize, double defaultParam)
{
    auto env = std::getenv("cjCacheRatio");
    if (env == nullptr) {
        cacheRatio = defaultParam;
        return;
    }
    double size = CString::ParsePosDecFromEnv(env);
    if (size - minSize >= 0 && maxSize - size >= 0) {
        cacheRatio = size;
        return;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjCacheRatio parameter.Valid cjCacheRatio range is [%f, %f].\n",
            minSize, maxSize);
    }
    cacheRatio = defaultParam;
}
#endif

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr, MemMap& memoryOwner,
                               const HeapParam& heapParam, double garbageThreshold)
{
    const size_t metadataSize = GetMetadataSize(nUnit);
    InitializeSegments(regionInfoAddr, { MemoryRange{ regionInfoAddr + metadataSize, nUnit * RegionInfo::UNIT_SIZE } },
                       memoryOwner, heapParam, garbageThreshold);
}

void RegionManager::InitializeSegments(uintptr_t regionInfoAddr, const std::vector<MemoryRange>& inputRanges,
                                      MemMap& memoryOwner, const HeapParam& heapParam, double garbageThreshold)
{
    // OS reservations remain distinct for unreserve (notably on Windows),
    // while adjacent virtual ranges coalesce before receiving cache indices.
    std::vector<MemoryRange> reservations;
    for (const auto& range : inputRanges) {
        if (!reservations.empty() && reservations.back().End() == range.start) {
            reservations.back().size += range.size;
        } else {
            reservations.push_back(range);
        }
    }
    const size_t nUnit = RegionInfo::IndexedUnitCount(reservations);
    const size_t metadataSize = GetMetadataSize(nUnit);
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = reservations.front().start;
    this->regionHeapEnd = reservations.back().End();
    heapUnitCount = 0;
    for (const auto& range : reservations) {
        CHECK(memoryOwner.GetReservationRegistry().Contains(range.start, range.size));
        CHECK(inactiveRanges.RegisterRange(Range(range.start, range.size)));
        heapUnitCount += range.size / RegionInfo::UNIT_SIZE;
    }
    // zPageTable.cpp:37-52: address tables cover the highest available end,
    // while only the reservation registry supplies allocatable ranges.
    CHECK(ForwardingTable::Initialize(regionHeapStart, regionHeapEnd - regionHeapStart, RegionInfo::UNIT_SIZE));
    this->inactiveZone = regionHeapStart;
    activeUnitCount.store(0, std::memory_order_relaxed);
    SetMaxUnitCountForRegion(heapParam.regionSize);
    SetMaxUnitCountForPinnedRegion(heapParam.regionSize);
    SetLargeObjectThreshold(heapParam.regionSize);
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    RegionInfo::InitializeSegments(regionInfoAddr + metadataSize, reservations, &memoryOwner);
    freeRegionManager.Initialize(nUnit);
    this->exemptedRegionThreshold = heapParam.exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
}

void RegionManager::ScrubRememberedSetForRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    MAddress rStart = static_cast<MAddress>(region->GetRegionStart());
    MAddress rEnd = static_cast<MAddress>(region->GetRegionEnd());
    (void)Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, nullptr);
}

void RegionManager::DumpScrubCostAndReset(const char* point)
{
    (void)point;
}

void RegionManager::ReclaimRegion(RegionInfo* region)
{
    RegionInfo::RetirePage(region, [this, region] { ReclaimRetiredRegion(region); });
}

void RegionManager::ReclaimRetiredRegion(RegionInfo* region)
{
    // routedest: census, not a guard. The graft asked for CHECK(!IsRouteDestHeld()) here to
    // convert "I traced the paths" into a machine check, but none of the designs proved the
    // caller enumeration and five of the six ReclaimRegion callers have already detached the
    // region, so an abort here would trade an unproven assumption for a hard stop. Count and
    // name it instead, under the default-off account gate; a non-zero funnel_held is the
    // signal that the enumeration was wrong.
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RECLAIM_DIRTY);
    }
    // gcvroot Z2: poison reclaimed payload so use-after-free roots are identifiable (MRT_GCV2_ZAP_RECLAIM=1).
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, true });
}

bool RegionManager::StallAllocation(AllocationStallRequest& request, bool requestGc)
{
    if (requestGc) {
        bool anotherWave = false;
        do {
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            if (allocationStallBeforeWaveTestHook) {
                allocationStallBeforeWaveTestHook(*this);
            }
#endif
            const uint64_t waveBoundary = allocationStallQueue.CaptureWaveBoundary();
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
            if (allocationStallGcTestHook) {
                allocationStallGcTestHook(*this);
            } else
#endif
            {
                Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
            }
            SatisfyStalledAllocations();
            anotherWave = allocationStallQueue.CompleteWave(waveBoundary);
        } while (anotherWave);
    }

    ScopedEnterSaferegion enterSaferegion(false);
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    const bool satisfied = request.Wait(allocationStallBeforeWaitTestHook
        ? [this] { allocationStallBeforeWaitTestHook(*this); }
        : std::function<void()> {});
#else
    const bool satisfied = request.Wait();
#endif
    // Pair with the posting owner before the caller destroys its request.
    // zPageAllocator.cpp:1454-1464.
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    return satisfied;
}

void RegionManager::ReturnPageMemory(const PageMemory& memory)
{
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(memory.index));
    if (region != nullptr) {
        CHECK(region->GetUnitIdx() == memory.index && region->GetUnitCount() == memory.units);
        RegionInfo::RetirePage(region, [this, region, memory] {
            region->InitFreeUnits();
            ReturnRetiredPageMemory(memory);
        });
        return;
    }
    // An allocation cancelled before materialization has no page descriptor.
    // Reclaim/Release also arrive here after completing descriptor retirement.
    ReturnRetiredPageMemory(memory);
}

void RegionManager::ReturnRetiredPageMemory(const PageMemory& memory)
{
    // zPageAllocator.cpp:1999 / 2150: hand back memory, decrease used and
    // satisfy the FIFO in one allocator-owner critical section. Enter the
    // saferegion before the owner, including nested cache hand-back calls.
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    CHECK(memory.partition == 0);
    if (memory.committed) {
        freeRegionManager.AddGarbageUnits(memory.index, memory.units);
    } else {
        freeRegionManager.AddReleaseUnits(memory.index, memory.units);
    }
    const size_t bytes = memory.units * RegionInfo::UNIT_SIZE;
    CHECK(pageAllocatorUsed >= bytes);
    pageAllocatorUsed -= bytes;
    allocationStallQueue.SatisfyAvailableLocked([this](AllocationStallRequest& request) {
        return ClaimAllocationLocked(request);
    });
}

#if defined(MRT_ALLOCATION_STALL_OBSERVE)
void RegionManager::SetAllocationStallTestHooks(AllocationStallTestHook beforeWave,
                                                AllocationStallTestHook requestGc,
                                                AllocationStallTestHook beforeWait)
{
    allocationStallBeforeWaveTestHook = std::move(beforeWave);
    allocationStallGcTestHook = std::move(requestGc);
    allocationStallBeforeWaitTestHook = std::move(beforeWait);
}

size_t RegionManager::PendingStalledAllocations() const { return allocationStallQueue.Pending(); }
size_t RegionManager::EnqueuedStalledAllocations() const { return allocationStallQueue.EnqueuedCount(); }
size_t RegionManager::DequeuedStalledAllocations() const { return allocationStallQueue.DequeuedCount(); }
size_t RegionManager::SatisfiedStalledAllocations() const { return allocationStallQueue.SatisfiedCount(); }
size_t RegionManager::FailedStalledAllocations() const { return allocationStallQueue.FailedCount(); }
#endif

void RegionManager::SatisfyStalledAllocations()
{
    // zPageAllocator.cpp:2167: claim the actual resource and update used
    // under the ordinary allocation owner, then dequeue and notify.
    allocationStallQueue.SatisfyAvailable([this](AllocationStallRequest& request) {
        return ClaimAllocationLocked(request);
    });
}

bool RegionManager::ClaimAllocationLocked(AllocationStallRequest& request)
{
    const size_t size = request.GetSize();
    const size_t num = size / RegionInfo::UNIT_SIZE;
    PageMemory& memory = request.Memory();
    // Single logical partition for the current cache. A02c owns round-robin
    // selection and harvested multi-partition vmems (advisor 0913 03:4x).
    constexpr uint32_t partition = 0;
    if (!freeRegionManager.ClaimPageMemory(num, partition, memory)) {
        const Range range = inactiveRanges.ClaimLow(size);
        if (range.IsNull()) {
            return false;
        }
        const size_t index = RegionInfo::FindUnitIndex(range.Start());
        CHECK(index != std::numeric_limits<uint32_t>::max());
        inactiveZone.store(std::max(inactiveZone.load(std::memory_order_relaxed), range.End()),
                           std::memory_order_release);
        activeUnitCount.fetch_add(num, std::memory_order_release);
        memory = PageMemory{ index, num, partition, false };
    }
    if (!memory.committed) {
        Uncommitter::CancelCycleLocked();
    }
    pageAllocatorUsed += size;
    return true;
}

void RegionManager::ReclaimRegionToMarkQuarantine(RegionInfo* region)
{
    RegionInfo::RetirePage(region, [this, region] { ReclaimRetiredRegionToMarkQuarantine(region); });
}

void RegionManager::ReclaimRetiredRegionToMarkQuarantine(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());
    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RECLAIM_MARK_QUARANTINE);
    }
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lock(pageAllocatorMutex);
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
    CHECK(pageAllocatorUsed >= num * RegionInfo::UNIT_SIZE);
    pageAllocatorUsed -= num * RegionInfo::UNIT_SIZE;
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    const size_t size = region->GetRegionSize();
    RegionInfo::RetirePage(region, [this, region] { ReleaseRetiredRegion(region); });
    return size;
}

void RegionManager::ReleaseRetiredRegion(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.

    // holdercapture: large regions above the release threshold never reach CollectRegion,
    // so the snapshot has to be taken on this path too or the face is lost unrecorded.

    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    // Large regions above the release threshold bypass CollectRegion. Invalidate
    // their two owned bitmap slices before the address range can be unmapped/reused.
    ScrubRememberedSetForRegion(region);
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    {
        RegionInfo::InPlaceClaimScope drain(region, ZForwardingLife::Retire::RELEASE_REGION);
    }
    region->InitFreeUnits();
    {
        RegionInfo::ReleaseUnits(unitIndex, num);
    }
    ReturnPageMemory(PageMemory{ unitIndex, num, 0, false });
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    region->AddLiveCounts(1, obj->GetSize());
}

void RegionManager::AssembleSmallGarbageCandidates()
{
    fromRegionList.MergeRegionList(rawPointerPinnedRegionList, RegionInfo::RegionType::FROM_REGION);
    // twoflags: regions stamped post-mark-start of the previous major stay off from-space
    // until PrepareTrace clears the stamp (after this Assemble).
    {
        RegionInfo* region = recentFullRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextRegion();
            // routedest: a region a published route still names must not enter the collection
            // set. Unlike notRelocatableThisCycle this is not about liveness — the region may
            // well be dead — it is about address ownership: reclaiming it hands its units back
            // for ClearUnits while the route keeps answering the old geometry.
            if (!region->IsNotRelocatableThisCycle() &&
                !RouteDestHold::HoldsBack(region, RouteDestHold::Site::ASSEMBLE_RECENT_FULL)) {
                const size_t units = region->GetUnitCount();
                recentFullRegionList.DeleteRegion(region);
                RecentFullAccounting::Dequeue(1, units);
                fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            }
            region = next;
        }
    }
    {
        RegionInfo* region = unmovableFromRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextRegion();
            if (!region->IsNotRelocatableThisCycle() &&
                !RouteDestHold::HoldsBack(region, RouteDestHold::Site::ASSEMBLE_UNMOVABLE)) {
                unmovableFromRegionList.DeleteRegion(region);
                fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            }
            region = next;
        }
    }

    fromRegionList.VisitAllRegions([](RegionInfo* region) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->ClearLiveInfo(view);
    });
}

void RegionManager::AssembleLargeGarbageCandidates()
{
    oldLargeRegionList.MergeRegionList(recentLargeRegionList, RegionInfo::RegionType::LARGE_REGION);
    for (RegionInfo* region = oldLargeRegionList.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->ClearLiveInfo(view);
    }
}

void RegionManager::ClearNotRelocatableThisCycleFlags()
{
    auto clearList = [](RegionList& list) {
        list.VisitAllRegions([](RegionInfo* region) { region->SetNotRelocatableThisCycle(0); });
    };
    clearList(tlRegionList);
    clearList(recentFullRegionList);
    clearList(unmovableFromRegionList);
    clearList(fromRegionList);
    clearList(recentPinnedRegionList);
    clearList(oldPinnedRegionList);
    clearList(rawPointerPinnedRegionList);
    clearList(recentLargeRegionList);
    clearList(oldLargeRegionList);
    // Region caches may hold stamped regions until HandleTraceRegions merges them.
    clearList(fullTraceRegions);
    clearList(largeTraceRegions);
}

// routedest: drop the destination holds of one route generation. Called from
// PrepareFromRegionList, immediately after the ghost dispel walk and before the next
// generation's destinations are enrolled — placing it there rather than at the three
// PrepareForwardTable call sites is what makes it immune to a missed site, and there are
// three, two of them inside a single minor (WCollector.cpp:5117 and :5570) plus the major
// PostTrace one (:2124).
//
// Walks the same eleven lists as ClearNotRelocatableThisCycleFlags, and reports the gauge
// before clearing: holds that leak never get dropped and show up as monotonic growth in
// held_regions, which is the only way to tell that failure apart from the opposite one.
void RegionManager::ClearRouteDestHoldFlags()
{
    auto clearList = [](RegionList& list) {
        list.VisitAllRegions([](RegionInfo* region) {
            if (region->IsRouteDestHeld()) {
                region->SetRouteDestHold(0);
            }
        });
    };
    clearList(tlRegionList);
    clearList(recentFullRegionList);
    clearList(unmovableFromRegionList);
    clearList(fromRegionList);
    clearList(recentPinnedRegionList);
    clearList(oldPinnedRegionList);
    clearList(rawPointerPinnedRegionList);
    clearList(recentLargeRegionList);
    clearList(oldLargeRegionList);
    clearList(fullTraceRegions);
    clearList(largeTraceRegions);
}

void RegionManager::AssemblePinnedGarbageCandidates(bool collectAll)
{
    oldPinnedRegionList.MergeRegionList(recentPinnedRegionList, RegionInfo::RegionType::FULL_PINNED_REGION);
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* nextRegion = region->GetNextRegion();
        if (collectAll && (region->GetRawPointerObjectCount() > 0)) {
            oldPinnedRegionList.DeleteRegion(region);
            rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
        }
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->ClearLiveInfo(view);
        region = nextRegion;
    }
}

// ThreadLocalAllocBuffer::initial_desired_size (cpp:265): a new thread
// starts with the published allocation fraction instead of a fixed extent.
void RegionManager::InitializeTLAB(AllocBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    buffer.ResizeTLAB(GetTLABCapacity(), tlabRequestedFraction.Average() / threads,
                      GetThreadLocalRegionSize());
}

// ZTLABUsage::reset (zTLABUsage.cpp:41), called before retiring allocating
// regions in young mark-start (zGeneration.cpp:862).
void RegionManager::ResetTLABUsage()
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t used = tlabUsed.exchange(0, std::memory_order_relaxed);
    if (used != 0) {
        // TruncatedSeq::davg uses AbsSeq's exponential average, alpha=0.3;
        // its last value and average are stable throughout the next cycle.
        tlabCapacity = lastTLABUsed == 0 ? used : tlabCapacity + 0.3 * (used - tlabCapacity);
        lastTLABUsed = used;
    }
}

// ZThreadLocalAllocBuffer::publish_statistics (zThreadLocalAllocBuffer.cpp:52).
// Thread retirement statistics consume the already published backing history.
void RegionManager::PublishTLABStatistics()
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t capacity = GetTLABCapacity();
    TLABStatistics total = retiredTLABStatistics;
    retiredTLABStatistics = TLABStatistics{};
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    const double fallback = tlabRequestedFraction.Average() / threads;
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([&](AllocBuffer& buffer) {
        buffer.AccumulateTLABStatistics(total, GetTLABUsed(), capacity);
        buffer.ResizeTLAB(capacity, fallback, GetThreadLocalRegionSize());
    });
    if (total.Used() != 0) {
        tlabAllocatingThreads.Sample(total.allocatingThreads);
        if (lastTLABUsed > 0.5 * capacity) {
            tlabRequestedFraction.Sample(std::min(static_cast<double>(total.Used()) /
                                                 std::max(capacity, size_t{1}), 1.0));
        }
    }
    VLOG(REPORT, "TLAB totals: used=%zu capacity=%zu allocated=%zu refills=%zu refill-waste=%zu gc-waste=%zu threads=%zu",
         lastTLABUsed, capacity, total.allocatedSize, total.refills, total.refillWaste, total.gcWaste,
         total.allocatingThreads);
}

void RegionManager::RetireTLABStatistics(AllocBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    buffer.AccumulateTLABStatistics(retiredTLABStatistics, GetTLABUsed(), GetTLABCapacity());
}

YoungCollectionStats RegionManager::PrepareYoungGarbageCandidates(const std::function<void(RegionInfo*)>& visitor)
{
    PublishTLABStatistics();
    YoungCollectionStats stats;
    uint64_t subStart = TimeUtil::NanoSeconds();
    RegionInfo* oldRegion = fromRegionList.GetHeadRegion();
    while (oldRegion != nullptr) {
        RegionInfo* next = oldRegion->GetNextRegion();
        ++stats.fromVisited;
        stats.fromVisitedUnits += oldRegion->GetUnitCount();
        fromRegionList.DeleteRegion(oldRegion);
        ParkUnmovableFromRegion(oldRegion);
        oldRegion = next;
    }
    stats.reparkNs = TimeUtil::NanoSeconds() - subStart;

    subStart = TimeUtil::NanoSeconds();
    RegionInfo* region = unmovableFromRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        ++stats.unmovableVisited;
        stats.unmovableVisitedUnits += region->GetUnitCount();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        ++stats.unmovableYoung;
        // twoflags: notRelocatable is major-Assemble only. Young mark re-establishes
        // liveness for POST_TRACE-stamped regions — do not skip minor CSet.
        // routedest: that reasoning is about liveness and does not transfer. A route
        // destination is excluded here on address ownership, not on whether its contents are
        // reachable. This loop matters most of the four: every mutator thread-local region is
        // young (RegionSpace.cpp takes the youngRegion = true default), and the destination
        // recorded at RegionManager.cpp:1957 is exactly such a region — so before this gate a
        // minor collected a live route's destination while honouring nothing.
        const uint64_t holdStart = TimeUtil::NanoSeconds();
        const bool held = RouteDestHold::HoldsBack(region, RouteDestHold::Site::YOUNG_UNMOVABLE);
        stats.holdCheckNs += TimeUtil::NanoSeconds() - holdStart;
        if (held) {
            ++stats.unmovableHeld;
            region = next;
            continue;
        }
        MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
        const uint64_t clearStart = TimeUtil::NanoSeconds();
        region->ClearLiveInfo(view);
        stats.clearLiveNs += TimeUtil::NanoSeconds() - clearStart;
        ++stats.clearLiveRegions;
        stats.clearLiveUnits += region->GetUnitCount();
        const uint64_t visitorStart = TimeUtil::NanoSeconds();
        visitor(region);
        stats.visitorNs += TimeUtil::NanoSeconds() - visitorStart;
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() == 0) {
            const uint64_t moveStart = TimeUtil::NanoSeconds();
            unmovableFromRegionList.DeleteRegion(region);
            fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            stats.listMoveNs += TimeUtil::NanoSeconds() - moveStart;
        }
        region = next;
    }
    stats.unmovableNs = TimeUtil::NanoSeconds() - subStart;

    subStart = TimeUtil::NanoSeconds();
    region = recentFullRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        ++stats.recentFullVisited;
        stats.recentFullVisitedUnits += region->GetUnitCount();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        ++stats.recentFullYoung;
        // routedest: same exclusion as the unmovable young loop above.
        const uint64_t holdStart = TimeUtil::NanoSeconds();
        const bool held = RouteDestHold::HoldsBack(region, RouteDestHold::Site::YOUNG_RECENT_FULL);
        stats.holdCheckNs += TimeUtil::NanoSeconds() - holdStart;
        if (held) {
            ++stats.recentFullHeld;
            region = next;
            continue;
        }
        MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
        const uint64_t clearStart = TimeUtil::NanoSeconds();
        region->ClearLiveInfo(view);
        stats.clearLiveNs += TimeUtil::NanoSeconds() - clearStart;
        ++stats.clearLiveRegions;
        stats.clearLiveUnits += region->GetUnitCount();
        const uint64_t visitorStart = TimeUtil::NanoSeconds();
        visitor(region);
        stats.visitorNs += TimeUtil::NanoSeconds() - visitorStart;
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() != 0) {
            region = next;
            continue;
        }
        const size_t units = region->GetUnitCount();
        const uint64_t moveStart = TimeUtil::NanoSeconds();
        recentFullRegionList.DeleteRegion(region);
        RecentFullAccounting::Dequeue(1, units);
        fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        stats.listMoveNs += TimeUtil::NanoSeconds() - moveStart;
        region = next;
    }
    stats.recentFullNs = TimeUtil::NanoSeconds() - subStart;
    return stats;
}

void RemoveRegionLocked(RegionList* regionList, RegionInfo* region)
{
    regionList->DeleteRegionLocked(region);
}

namespace {
// Claim FROM under the from-list lock. AddRawPointerObject may retype to
// PINNED after ExemptFromRegions snapshots the list (RegionManager.h:507;
// CI face del->IsFromRegion at post_trace). ZGC skips !is_relocatable
// (zGeneration.cpp:211-213); a lost claim is the same skip, not a relaxed CHECK.
bool ClaimFromRegion(RegionList& fromList, RegionInfo* del, RegionInfo::RegionType newType, const char* site)
{
    if (fromList.TryDeleteRegion(del, RegionInfo::RegionType::FROM_REGION, newType)) {
        return true;
    }
    const unsigned t = static_cast<unsigned>(del->GetRegionType());
    const unsigned rs = static_cast<unsigned>(del->RelocateObserve());
    LOG(RTLOG_ERROR, "[GCV2][isfromreg] site=%s skip type=%u route=%u young=%u", site, t, rs,
        static_cast<unsigned>(del->IsYoungRegion()));
    CHECK_DETAIL(del->GetRegionType() == RegionInfo::RegionType::RAW_POINTER_PINNED_REGION ||
                     del->GetRegionType() == RegionInfo::RegionType::UNMOVABLE_FROM_REGION ||
                     del->GetRegionType() == RegionInfo::RegionType::GARBAGE_REGION,
                 "[isfromreg] site=%s unexpected type=%u route=%u", site, t, rs);
    return false;
}

std::atomic<size_t> g_fwdToGateRefuse{ 0 };
std::atomic<bool> g_fwdToGateAtexit{ false };

void NoteFwdToGateRefuse(const char* site, BaseObject* toObj)
{
    const size_t n = g_fwdToGateRefuse.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_fwdToGateAtexit.exchange(true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr, "[GCV2][fwd-to-gate] atexit refuse=%zu\n",
                         g_fwdToGateRefuse.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
    if (n <= 8 || (n & (n - 1)) == 0) {
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        LOG(RTLOG_ERROR, "[GCV2][fwd-to-gate] refuse n=%zu site=%s to=%p phase=%s", n, site,
            static_cast<void*>(toObj), Collector::GetGCPhaseName(phase));
    }
}
} // namespace

// ZGC zGeneration.cpp:211-213: !is_relocatable (is_allocating) pages are not
// registered with the selector. HasMarkStartAllocGap ≡ zPage.inline.hpp:180-185.
// Called at CSet select (ExemptFromRegions) and again before PrepareForwardable
// so a watermark-gap region never publishes a route (915e6348 ghost).
size_t RegionManager::ExemptMarkStartAllocatingFromCSet()
{
    static std::atomic<size_t> g_armed{ 0 };
    static std::atomic<size_t> g_turned{ 0 };
    static std::atomic<bool> atexitOn{ false };
    if (!atexitOn.exchange(true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr, "[GCV2][markwater] atexit armed=%zu turned=%zu\n",
                         g_armed.load(std::memory_order_relaxed),
                         g_turned.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
    std::vector<RegionInfo*> snapshot;
    fromRegionList.VisitAllRegions([&snapshot](RegionInfo* r) { snapshot.push_back(r); });
    size_t armed = 0;
    size_t turned = 0;
    for (RegionInfo* fromRegion : snapshot) {
        if (fromRegion == nullptr || !fromRegion->HasMarkStartAllocGap()) {
            continue;
        }
        ++armed;
        if (!ClaimFromRegion(fromRegionList, fromRegion, RegionInfo::RegionType::UNMOVABLE_FROM_REGION, "markwater")) {
            continue;
        }
        DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) markwater skip CSet: %zu units, %zu live bytes",
             fromRegion, fromRegion->GetRegionStart(), fromRegion->GetRegionAllocatedSize(),
             fromRegion->GetRegionEnd(), fromRegion->GetUnitCount(), fromRegion->GetLiveByteCount());
        fromRegion->PreserveRetainedLiveInfo();
        ExemptFromRegion(fromRegion);
        ++turned;
    }
    if (armed != 0) {
        g_armed.fetch_add(armed, std::memory_order_relaxed);
    }
    if (turned != 0) {
        g_turned.fetch_add(turned, std::memory_order_relaxed);
    }
    if (armed != 0 || turned != 0) {
        LOG(RTLOG_ERROR,
            "[GCV2][markwater] cset-skip armed=%zu turned=%zu tot_armed=%zu tot_turned=%zu",
            armed, turned, g_armed.load(std::memory_order_relaxed),
            g_turned.load(std::memory_order_relaxed));
    }
    return turned;
}

// Cost-model CSet (ZRelocationSetSelector.cpp:114-196) after mark, before flip.
// Sort key = GetLiveByteCount(); stop = relative reclaimable <= kRelocationFragmentationLimitPercent.
size_t RegionManager::ExemptFromRegions()
{
    CsetEmptyWho::BeginCycle();
    (void)ExemptMarkStartAllocatingFromCSet();
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    double exempt = exemptedRegionThreshold;
    rawPointerPinnedRegionList.VisitAllRegions([](RegionInfo* region) {
        if (region->GetLiveByteCount() > 0) {
            region->PreserveRetainedLiveInfoUpTo(
                std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
        }
    });
    std::vector<RegionInfo*> snapshot;
    fromRegionList.VisitAllRegions([&snapshot](RegionInfo* r) { snapshot.push_back(r); });
    std::vector<RelocRegionDesc> descs;
    std::vector<RegionInfo*> descRegions;
    descs.reserve(snapshot.size());
    descRegions.reserve(snapshot.size());
    for (RegionInfo* fromRegion : snapshot) {
        size_t liveBytes = fromRegion->GetLiveByteCount();
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        // zGeneration.cpp:216-221 register_empty_page iff !is_marked — sound
        // only because ZGC mark is complete (zPage.inline.hpp:223-225). Ours
        // is not: oldroots2 CsetEmptyWho (VisitHeapReferences + uncolor_bits +
        // derived) still NONE≈99.97% (derivedSeen=0). Freeing unmarked residual
        // dropped keep to 0 but SD256 N=6: 1×SEGV si_addr=0x8 trace_phase +
        // 1×checksum drift. Reverted. Bare liveBytes==0 mixes two classes:
        //   (1) dead from-copies — residual headers all FORWARDED.
        //   (2) unmarked residual — no incoming edge we can name, but mutator
        //       still observes them (SEGV/drift). Keep (2) for the selector.
        static constexpr bool kFreeEmptyAtCSetSelect = true;
        if (kFreeEmptyAtCSetSelect && liveBytes == 0 && rawPtrCnt == 0 &&
            !fromRegion->HasMarkStartAllocGap() && !fromRegion->IsYoungRegion()) {
            RegionInfo* del = fromRegion;
            const unsigned rs = static_cast<unsigned>(del->RelocateObserve());
            const unsigned ke = del->IsKnownEmpty(del->GetMarkView<Generation::Old>()) ? 1u : 0u;
            size_t residual = 0;
            size_t residualFwd = 0;
            size_t marked = 0;
            const uintptr_t start = del->GetRegionStart();
            const uintptr_t alloc = del->GetRegionAllocPtr();
            if (alloc > start && !del->IsLargeRegion()) {
                uintptr_t pos = start;
                while (pos < alloc) {
                    BaseObject* o = from_region_addr(pos);
                    if (!o->IsValidObject()) {
                        break;
                    }
                    const size_t sz = o->GetSize();
                    if (sz == 0) {
                        break;
                    }
                    ++residual;
                    if (o->IsForwarded()) {
                        ++residualFwd;
                    }
                    if (del->IsMarkedObject(del->GetMarkView<Generation::Old>(), o)) {
                        ++marked;
                    }
                    pos += sz;
                }
            }
            const bool deadFromCopy = residual == residualFwd;
            // zGeneration.cpp:216-221 register_empty_page iff !is_marked.
            // Held until in-place claim waits readers even at fwdRefCount==0
            // (LEAD-NOTE 0820 21:1x / PORT_ZFORWARDING step 3). oldroots2
            // 152ccd59 SEGV+drift was ClearUnits racing a naked mutator ref.
            const bool unmarkedResidual = residual != 0 && marked == 0;
            const bool freeEmpty = (ke != 0) || deadFromCopy || unmarkedResidual;
            {
                static std::atomic<size_t> gCsetEmpty{ 0 };
                static std::atomic<size_t> gCsetEmptyResidual{ 0 };
                static std::atomic<size_t> gCsetEmptyMarked{ 0 };
                static std::atomic<size_t> gCsetEmptyKeep{ 0 };
                static std::atomic<bool> gCsetEmptyAtexit{ false };
                const size_t n = gCsetEmpty.fetch_add(1, std::memory_order_relaxed) + 1;
                if (residual != 0) {
                    gCsetEmptyResidual.fetch_add(1, std::memory_order_relaxed);
                }
                if (marked != 0) {
                    gCsetEmptyMarked.fetch_add(1, std::memory_order_relaxed);
                }
                if (!freeEmpty) {
                    gCsetEmptyKeep.fetch_add(1, std::memory_order_relaxed);
                }
                if (!gCsetEmptyAtexit.exchange(true, std::memory_order_relaxed)) {
                    std::atexit([]() {
                        std::fprintf(stderr,
                                     "[WHODEAD][cset-empty] atexit n=%zu residualPages=%zu markedPages=%zu keep=%zu\n",
                                     gCsetEmpty.load(std::memory_order_relaxed),
                                     gCsetEmptyResidual.load(std::memory_order_relaxed),
                                     gCsetEmptyMarked.load(std::memory_order_relaxed),
                                     gCsetEmptyKeep.load(std::memory_order_relaxed));
                        std::fflush(stderr);
                    });
                }
                if (n <= 8 || (n & (n - 1)) == 0) {
                    LOG(RTLOG_ERROR,
                        "[WHODEAD][cset-empty] n=%zu region=%p start=%#zx live=%zu residual=%zu fwd=%zu marked=%zu "
                        "route=%u ke=%u ghost=%u alloc=%u reason=%u free=%u",
                        n, del, start, liveBytes, residual, residualFwd, marked, rs, ke,
                        static_cast<unsigned>(del->IsGhostFromRegion()),
                        static_cast<unsigned>(del->HasMarkStartAllocGap()),
                        static_cast<unsigned>(Heap::GetHeap().GetCollector().GetGCStats().reason),
                        static_cast<unsigned>(freeEmpty));
                }
            }
            if (!freeEmpty) {
                CsetEmptyWho::NoteKeep(del, residual, residualFwd, marked);
                continue;
            }
            if (!ClaimFromRegion(fromRegionList, del, RegionInfo::RegionType::GARBAGE_REGION, "cset-empty")) {
                continue;
            }
            if (del->GetRawPointerObjectCount() > 0) {
                rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
                continue;
            }

            TraceClear::NoteRange(del->GetRegionStart(), del->GetRegionSize(),
                                  residual != 0 ? "coll_live" : "coll_empty", del, liveBytes,
                                  static_cast<unsigned>(Generation::Old),
                                  0);
            ScrubRememberedSetForRegion(del);
            garbageRegionList.PrependRegion(del, RegionInfo::RegionType::GARBAGE_REGION);
            continue;
        }
        if (rawPtrCnt > 0) {
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) pinned by forwarding: %zu units, %zu live bytes rawPtr cnt %u",
                del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount(), rawPtrCnt);
            if (!ClaimFromRegion(fromRegionList, del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION, "cset-rawpin")) {
                continue;
            }
            if (liveBytes > 0) {
                del->PreserveRetainedLiveInfo();
            }
            rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
            continue;
        }
        if (!kUseRelocationSetSelector) {
            size_t threshold = static_cast<size_t>(exempt * fromRegion->GetRegionSize());
            if (liveBytes > threshold) {
                RegionInfo* del = fromRegion;
                if (!ClaimFromRegion(fromRegionList, del, RegionInfo::RegionType::UNMOVABLE_FROM_REGION, "cset-thresh")) {
                    continue;
                }
                del->PreserveRetainedLiveInfo();
                ExemptFromRegion(del);
                floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
            }
            continue;
        }
        RelocRegionDesc d;
        d.liveBytes = liveBytes;
        d.capacity = fromRegion->GetRegionSize();
        d.kind = fromRegion->IsLargeRegion() ? RelocRegionKind::Large : RelocRegionKind::Small;
        d.id = static_cast<uint32_t>(descs.size());
        d.allocating = fromRegion->HasMarkStartAllocGap();
        descs.push_back(d);
        descRegions.push_back(fromRegion);
    }
    if (kUseRelocationSetSelector) {
        const RelocSelectResult selected = SelectRelocationSet(descs);
        std::vector<char> keep(descs.size(), 0);
        for (uint32_t id : selected.selectedIds) {
            if (id < keep.size()) {
                keep[id] = 1;
            }
        }
        for (size_t i = 0; i < descs.size(); ++i) {
            if (keep[i] != 0) {
                continue;
            }
            RegionInfo* del = descRegions[i];
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) exempted by relocsel: %zu units, %zu live bytes", del,
                del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount());
            if (!ClaimFromRegion(fromRegionList, del, RegionInfo::RegionType::UNMOVABLE_FROM_REGION, "cset-relocsel")) {
                continue;
            }
            // ZGC keeps an unselected relocation-set page in place; its liveness
            // snapshot is only required when this cycle actually examined the
            // page.  Relocsel also sees pages with a live-byte census but no
            // current mark face (NEVER_EXAMINED), so use the bounded preserve
            // form rather than asserting that every live page has a snapshot.
            del->PreserveRetainedLiveInfoUpTo(
                std::min(del->GetCensusBoundary(), del->GetRegionAllocPtr()));
            ExemptFromRegion(del);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        }
    }

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t exemptedFromBytes = unmovableFromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "exempt from-space: %zu B - %zu B -> %zu B, %zu B floating garbage, %zu B to forward",
         oldFromBytes, exemptedFromBytes, newFromBytes, floatingGarbage, forwardBytes);
    return newFromBytes - forwardBytes;
}

void RegionManager::ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                                     bool skipKnownEmptyRegions) const
{
    VisitPageOwners([&](RegionInfo* region) {
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            return;
        }
        MarkView<Generation::Old> oldView = region->GetMarkView<Generation::Old>();
        if (skipKnownEmptyRegions && region->IsKnownEmpty(oldView)) {
            return;
        }
        region->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
    });
}

void RegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

void RegionManager::StampCensusBoundaries()
{
    VisitPageOwners([&](RegionInfo* region) {
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            region->StampCensusBoundary();
        }
    });
}

void RegionManager::PromoteAllRegions()
{
    VisitPageOwners([&](RegionInfo* region) {
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            size_t liveBytes = region->GetLiveByteCount();
            if (liveBytes > 0) {
                region->PreserveRetainedLiveInfoUpTo(
                    std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
            } else if (region->GetRawPointerObjectCount() == 0) {
                region->PreserveRetainedLiveInfo(region->GetRegionStart());
            }
            if (region->IsYoungRegion()) {
                MarkView<Generation::Young> youngView = region->GetMarkView<Generation::Young>();
                (void)region->PromoteYoungRegion(youngView);
            } else {
                // Preserve the pre-genface cleanup for already-old regions.
                region->SetYoungAge(0);
            }
        }
    });
}

RegionInfo* RegionManager::TakeRegion(size_t num, RegionInfo::UnitRole type, bool expectPhysicalMem,
                                      bool allowSaferegion, bool clearPayload)
{
    // a chance to invoke heuristic gc.
    // routefix: under ROUTING, skip RequestGC — PostIgnoredGcRequest may ScopedEnterSaferegion.
    if (allowSaferegion && !Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        GCStats& gcStats = collector.GetGCStats();
        size_t heapThreshold = gcStats.GetThreshold();
        size_t youngRegionTriggerBytes = kGcTriggerYoungFixedBytes;
        if (kGcTriggerAllocRateEnabled && !kGcTriggerPinYoung32MB) {
            youngRegionTriggerBytes = gcStats.youngTriggerBytes.load(std::memory_order_acquire);
        }
        size_t youngAllocated = GetYoungAllocatedSize();
        size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
        // Occupancy young stays on the latched line (survival). Director uses the
        // 32MB now-gate so a new wave after a high-survival latch still minors
        // (12-wave NW). zDirector.cpp:296-306 / :331-381.
        const size_t directorMinorBytes = kGcTriggerYoungFixedBytes;
        bool requested = false;
        if (kGcTriggerAllocRateEnabled) {
            MutatorAllocRateStats rate = MutatorAllocRate::stats();
            const uint64_t nowNs = TimeUtil::NanoSeconds();
            const uint64_t prevFinish = GCStats::GetPrevGCFinishTime();
            const uint64_t sinceNs = nowNs > prevFinish ? nowNs - prevFinish : 0;
            GcTriggerInputs in;
            in.allocRateAvgBps = rate.avg;
            in.allocRatePredictBps = rate.predict;
            in.allocRateSdBps = rate.sd;
            in.usedBytes = allocated;
            in.youngUsedBytes = youngAllocated;
            in.capacityBytes = Heap::GetHeap().GetMaxCapacity();
            in.softMaxBytes = MutatorAllocRate::soft_max_heap_size();
            in.lastGcDurationSec =
                static_cast<double>(gcStats.lastGcDurationNs.load(std::memory_order_relaxed)) /
                static_cast<double>(SECOND_TO_NANO_SECOND);
            in.timeSinceLastGcSec = static_cast<double>(sinceNs) / static_cast<double>(SECOND_TO_NANO_SECOND);
            in.collectionIntervalSec = 0.0;
            in.warmupCyclesDone = gcStats.warmupCyclesDone.load(std::memory_order_relaxed);
            in.isWarm = gcStats.isWarm.load(std::memory_order_relaxed);
            in.isTimeTrustable = gcStats.isTimeTrustable.load(std::memory_order_relaxed);
            if constexpr (kGcTriggerProactiveEnabled || kGcTriggerDynamicWorkersEnabled) {
                in.lastYoungGcDurationSec = GCStats::lastYoungGcDurationAvgSec.load(std::memory_order_relaxed);
                in.lastOldGcDurationSec = GCStats::lastOldGcDurationAvgSec.load(std::memory_order_relaxed);
            }
            if constexpr (kGcTriggerProactiveEnabled) {
                const uint64_t lastMajorNs = GCStats::lastMajorFinishNs.load(std::memory_order_relaxed);
                const uint64_t sinceMajorNs =
                    (lastMajorNs == 0 || nowNs <= lastMajorNs) ? sinceNs : nowNs - lastMajorNs;
                in.timeSinceLastMajorSec =
                    static_cast<double>(sinceMajorNs) / static_cast<double>(SECOND_TO_NANO_SECOND);
                in.usedAtLastMajorEnd = GCStats::usedAtLastMajorEnd.load(std::memory_order_relaxed);
            }
            if constexpr (kGcTriggerMajorAllocRateEnabled) {
                in.oldUsedBytes = allocated > youngAllocated ? allocated - youngAllocated : 0;
                in.lastYoungGcDurationSec = GCStats::lastYoungGcDurationAvgSec.load(std::memory_order_relaxed);
                in.lastOldGcDurationSec = GCStats::lastOldGcDurationAvgSec.load(std::memory_order_relaxed);
                in.totalCollections = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
                in.collectionsAtLastMajor = GCStats::collectionsAtLastMajor.load(std::memory_order_relaxed);
                in.oldLiveAtMarkEnd = GCStats::oldLiveAtMarkEnd.load(std::memory_order_relaxed);
                in.reclaimedPerYoungAvg = GCStats::reclaimedPerYoungAvg.load(std::memory_order_relaxed);
                in.reclaimedPerOldAvg = GCStats::reclaimedPerOldAvg.load(std::memory_order_relaxed);
            }
            const GcTriggerDecision d = DecideGcTrigger(in);
            if constexpr (kGcTriggerDynamicWorkersEnabled) {
                const uint32_t poolCap = static_cast<uint32_t>(
                    std::max(Heap::GetHeap().GetCollectorResources().GetGCThreadCount(false), 1));
                const double lastWorkers =
                    static_cast<double>(g_gcTriggerYoungWorkers.load(std::memory_order_relaxed));
                const GcWorkerSelection workers = SelectGcWorkers(in, poolCap, lastWorkers);
                g_gcTriggerYoungWorkers.store(workers.youngWorkers, std::memory_order_relaxed);
                g_gcTriggerOldWorkers.store(workers.oldWorkers, std::memory_order_relaxed);
            }
            g_gcTriggerArmed.fetch_add(1, std::memory_order_relaxed);
            if (d.kind == GcTriggerKind::MAJOR) {
                g_gcTriggerTurned.fetch_add(1, std::memory_order_relaxed);
                NoteGcTriggerRule(d.rule);
                DLOG(ALLOC, "request heu gc via DecideGcTrigger rule=%d used=%zu cap=%zu",
                     static_cast<int>(d.rule), allocated, in.capacityBytes);
                collector.RequestGC(GC_REASON_HEU, true);
                requested = true;
            } else if (ShouldRequestDirectorMinor(d.kind, youngAllocated, directorMinorBytes)) {
                // zDirector.cpp:331-381 — alloc-rate / high-usage keep evaluating after
                // the occupancy watermark has been raised. Occupancy young still uses
                // the latched line; director uses the 5%/32MB now-gate so a new young
                // wave is collected (12-wave NW). is_young_small is already inside
                // RuleAllocRate / RuleHighUsage (zDirector.cpp:342-343, :371-372).
                g_gcTriggerTurned.fetch_add(1, std::memory_order_relaxed);
                NoteGcTriggerRule(d.rule);
                DLOG(ALLOC, "request young gc via DecideGcTrigger rule=%d young=%zu trigger=%zu",
                     static_cast<int>(d.rule), youngAllocated, youngRegionTriggerBytes);
                collector.RequestGC(GC_REASON_YOUNG, true);
                requested = true;
            }
        }
        if (!requested && youngAllocated >= youngRegionTriggerBytes) {
            DLOG(ALLOC, "request young gc: allocated %zu, threshold %zu", youngAllocated, youngRegionTriggerBytes);
            collector.RequestGC(GC_REASON_YOUNG, true);
            requested = true;
        }
        if (!requested && allocated >= heapThreshold) {
            DLOG(ALLOC, "request heu gc: allocated %zu, threshold %zu", allocated, heapThreshold);
            collector.RequestGC(GC_REASON_HEU, true);
        }
    }

    // check for allocation since we do not want gc threads and mutators do any harm to each other.
    size_t size = num * RegionInfo::UNIT_SIZE;
    // routefix: RequestForRegion may sleep; under ROUTING keep the critical section short.
    if (allowSaferegion) {
        RequestForRegion(size);
    }

#if !defined(__OHOS__)
    size_t gatedBytes = 0;
    RegionInfo* garbage = allowSaferegion ? TakeReclaimableGarbageRegion(&gatedBytes) : nullptr;
    if (garbage != nullptr) {
        ReclaimRegion(garbage);
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

    AllocationStallRequest request(size, static_cast<uint8_t>(type), expectPhysicalMem, clearPayload);
    bool claimed = false;
    bool requestGc = false;
    {
        std::lock_guard<std::mutex> lock(pageAllocatorMutex);
        claimed = ClaimAllocationLocked(request);
        if (!claimed && allowSaferegion && !IsGcThread()) {
            requestGc = allocationStallQueue.EnqueueLocked(request);
        }
    }
    if (!claimed && allowSaferegion && !IsGcThread()) {
        claimed = StallAllocation(request, requestGc);
    }
    if (claimed) {
        size_t committedUnits = 0;
        RegionInfo* region = freeRegionManager.MaterializePageMemory(
            request.Memory(), type, request.ExpectsPhysicalMemory(), request.ClearsPayload(), committedUnits);
        if (region == nullptr) {
            // zPageAllocator.cpp:1906: preserve the succeeded prefix in the
            // committed cache and return only the failed suffix uncommitted.
            // No page descriptor has been published for this allocation.
            ScopedEnterSaferegion enterSaferegion(true);
            std::lock_guard<std::mutex> lock(pageAllocatorMutex);
            const size_t index = request.Memory().index;
            if (committedUnits != 0) {
                freeRegionManager.AddGarbageUnits(index, committedUnits);
            }
            freeRegionManager.AddReleaseUnits(index + committedUnits, num - committedUnits);
            CHECK(pageAllocatorUsed >= size);
            pageAllocatorUsed -= size;
            allocationStallQueue.SatisfyAvailableLocked([this](AllocationStallRequest& pending) {
                return ClaimAllocationLocked(pending);
            });
            return nullptr;
        }
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        MutatorAllocRate::sample_allocation(size);
        return region;
    }

    if (gatedBytes > 0) {
        static std::atomic<size_t> supplyGatedPressureCount { 0 };
        size_t n = supplyGatedPressureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            VLOG(REPORT, "[Alloc] supply_gated_pressure gated_bytes=%zu n=%zu", gatedBytes, n);
        }
    }
    return nullptr;
}

template<Generation G>
void RegionManager::StartForwardFromRegions(GCWorkers& workers)
{
    CHECK(!relocationStarted);
    relocationStarted = true;
    relocationDrained = false;
    relocationWorkers = &workers;
    relocationRequestQueue.BeginWorkers(workers.ActiveWorkers());
}

template<Generation G>
void RegionManager::DrainForwardFromRegions()
{
    if (relocationDrained) {
        return;
    }
    relocationDrained = true;
    if (relocationWorkers == nullptr) {
        ForwardFromRegions<G>();
        return;
    }
    ForwardTask<G> task(*this, fromRegionList);
    relocationWorkers->Run(task);
}

template<Generation G>
void RegionManager::ForwardFromRegions(GCWorkers& workers)
{
    if (!relocationStarted) {
        StartForwardFromRegions<G>(workers);
    }
    DrainForwardFromRegions<G>();
    relocationWorkers = nullptr;
    relocationStarted = false;
    relocationDrained = false;
}

template<Generation G>
void RegionManager::ForwardClaimedPage(RegionInfo* region, ForwardingTable::Owner owner, bool claimed)
{
    if (!owner || (!claimed && !owner->claim())) return;
    ZForwardingLife::PageWorkScope work(owner.get());
    ForwardRegion<G>(region);
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(12, region, nullptr);
#endif
    // All page metadata and legacy helper work is finished. A nested drain
    // may already have consumed the construction token; otherwise drop it now.
    if (owner->ref_count().load(std::memory_order_acquire) != 0) owner->release_page();
    owner->detach_page();
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(10, region, nullptr);
#endif
    owner->mark_done();
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(5, region, nullptr);
#endif
    // From here on only forwarding/queue state may be touched.
    (void)relocationRequestQueue.Complete(owner.get());
}


namespace {
void WaitCopiedObjectsUnlocked(RegionInfo* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return;
    }
    ZForwardingLife::WaitPageDone(region->PeekForwardingOwner());
}

template<typename Fn>
void ForEachLiveObjectStart(RegionInfo* region, MAddress start, MAddress allocPtr, Fn&& fn)
{
    const size_t regionBytes = allocPtr > start ? static_cast<size_t>(allocPtr - start) : 0;
    size_t offset = 0;
    while (offset < regionBytes) {
        BaseObject* object = from_region_addr(start + offset);
        if (!Collector::PlausibleManagedObjectGate("ForEachLiveObjectStart", object)) {
            break;
        }
        const size_t size = RegionSpace::GetAllocSize(*object);
        if (size == 0 || size > regionBytes - offset) {
            break;
        }
        if (region->IsOwnerSurvivedObject(offset)) {
            region->RecordRouteStart(offset);
            fn(object, offset);
        }
        offset += size;
    }
}

// ZGC's relocate() marks a forwarding life done only after every survivor has
// a forwarding receipt (zRelocate.cpp:1137-1153). Header state and compact
// geometry are not receipts: kept/in-place survivors must have an explicit
// from->from entry in the same active/retired table. Keep this check at the
// producer boundary so a receipt-less publication fails loudly.
bool VerifyForwardingReceiptsClosed(RegionInfo* region, const char* site)
{
    if (region == nullptr || !region->IsGhostFromRegion()) {
        return true;
    }
    const RegionInfo::RouteStartTable* starts = region->LoadRouteStartTable();
    if (starts == nullptr) {
        CHECK_DETAIL(!region->HasFromPageMetadata(),
                     "%s missing exact-start set before forwarding done region=%p", site, region);
        return true;
    }
    const MAddress start = region->GetRegionStart();
    const MAddress regionEnd = region->GetRegionEnd();
    ZForwarding* active = ForwardingTable::RetainPageOwner(region).get();
    const ZForwarding::FromPageView* fromPage = active == nullptr ? nullptr : active->from_page_snapshot();
    const MAddress frozenTop = fromPage == nullptr ? regionEnd : fromPage->topAtStart;
    size_t survivors = 0;
    size_t receipts = 0;
    for (const auto& entry : *starts) {
        const size_t offset = entry.first;
        // After CompactRegion the bump pointer is the packed top, not the
        // original from-range. Exact starts are from-offsets; bound by the
        // region, not the post-compact allocPtr.
        if (entry.second == 0 || frozenTop < start ||
            offset >= static_cast<size_t>(frozenTop - start)) {
            continue;
        }
        ++survivors;
        const MAddress from = start + offset;
        const ForwardingTable::LookupResult lookup = ForwardingTable::LookupForwarding(from, ForwardingTable::RetainPageOwner(region).get());
        const bool hit = lookup.to != 0 &&
            lookup.answer == ForwardingTable::ToAnswer::ArmedHit;
        CHECK_DETAIL(hit,
                     "%s receipt gap region=%p exactStart=%#zx answer=%u route=%u fwdDone=%u refs=%d copy=%d",
                     site, region, static_cast<size_t>(from), static_cast<unsigned>(lookup.answer),
                     static_cast<unsigned>(region->RelocateObserve()),
                     static_cast<unsigned>(region->IsForwardingDone()), region->ForwardingRefCount(),
                      region->CopyInflightWord());
        if (hit) {
            ++receipts;
        }
    }
    CHECK_DETAIL(receipts == survivors,
                 "%s receipt count mismatch region=%p survivors=%zu receipts=%zu route=%u fwdDone=%u refs=%d copy=%d",
                 site, region, survivors, receipts, static_cast<unsigned>(region->RelocateObserve()),
                 static_cast<unsigned>(region->IsForwardingDone()), region->ForwardingRefCount(),
                  region->CopyInflightWord());
    return true;
}
} // namespace

void RegionManager::ParkUnmovableFromRegion(RegionInfo* region)
{
    // youngconcfollow: callers already unlink the FROM node — TryDelete FROM here
    // would DecCounts a second time ("error count 1-0 16-0"). Only a GARBAGE node
    // can still sit on garbageRegionList (the CHECK at
    // TryTakeGarbageRegionAfterDispel, RegionManager.h:984); unlink it before the
    // rehome below so the garbage list cannot name a non-GARBAGE region.
    if (region != nullptr && region->IsGarbageRegion()) {
        garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                          RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    }
    unmovableFromRegionList.PrependRegion(region, RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
}

void RegionManager::ExemptFromRegion(RegionInfo* region)
{
    ParkUnmovableFromRegion(region);
}

namespace {
bool IncompleteRouteUnpublished(RegionInfo* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return false;
    }
    if (region->IsForwardingDone()) {
        return false;
    }
    return ForwardingTable::RetainPageOwner(region).get() != nullptr;
}
} // namespace

void RegionManager::FinishIncompleteFromRegions()
{
    // zRelocate.cpp:1041-1047: relocate() does not return with a half-copied page.
    std::vector<RegionInfo*> snap;
    auto push = [&snap](RegionInfo* region) {
        if (region != nullptr) {
            snap.push_back(region);
        }
    };
    ghostFromRegionList.VisitAllGhostRegions(push);
    fromRegionList.VisitAllRegions(push);
    unmovableFromRegionList.VisitAllRegions(push);
    garbageRegionList.VisitAllRegions(push);

    std::sort(snap.begin(), snap.end());
    snap.erase(std::unique(snap.begin(), snap.end()), snap.end());

    const bool young = Heap::GetHeap().GetCollector().GetGCStats().reason == GC_REASON_YOUNG;
    static std::atomic<size_t> g_zombieFinished{ 0 };
    static std::atomic<size_t> g_zombieKept{ 0 };
    size_t finished = 0;
    size_t kept = 0;

    for (RegionInfo* region : snap) {
        if (!IncompleteRouteUnpublished(region)) {
            continue;
        }
        if (region->IsUnmovableFromRegion()) {
            ++kept;
            continue;
        }
        if (region->IsGarbageRegion()) {
            garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                              RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
            ExemptFromRegion(region);
            ++kept;
            continue;
        }
        const bool wasFrom = region->IsFromRegion();
        if (wasFrom) {
            fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                           RegionInfo::RegionType::LONE_FROM_REGION);
        }
        const bool canForward = region->IsLoneFromRegion() ||
            (region->IsThreadLocalRegion() && (region->IsRoutingState() || region->IsCompacted()));
        if (canForward) {
            if (young) {
                ForwardRegion<Generation::Young>(region);
            } else {
                ForwardRegion<Generation::Old>(region);
            }
            if (!IncompleteRouteUnpublished(region)) {
                ++finished;
                continue;
            }
        }
        if (region->IsFromRegion()) {
            fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                           RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
        }
        if (region->IsLoneFromRegion() || region->IsFromRegion() || wasFrom) {
            ExemptFromRegion(region);
        }
        ++kept;
    }

    if (finished != 0) {
        g_zombieFinished.fetch_add(finished, std::memory_order_relaxed);
    }
    if (kept != 0) {
        g_zombieKept.fetch_add(kept, std::memory_order_relaxed);
    }
    const size_t finTot = g_zombieFinished.load(std::memory_order_relaxed);
    const size_t keptTot = g_zombieKept.load(std::memory_order_relaxed);
    if (finished != 0 || kept != 0 || finTot != 0 || keptTot != 0) {
        LOG(RTLOG_ERROR, "[GCV2][zombie] finished=%zu kept=%zu tot_finished=%zu tot_kept=%zu", finished, kept, finTot,
            keptTot);
    }

    for (RegionInfo* region : snap) {
        if (region == nullptr || region->IsFreeRegion()) {
            continue;
        }
        CHECK_DETAIL(!IncompleteRouteUnpublished(region),
                     "[GCV2][zombie] fourth state region=%p start=%#zx route=%u done=%u type=%u live=%zu "
                     "— cycle-end from-page not in {FORWARDED,COMPACTED,Exempt-kept}",
                     region, region->GetRegionStart(), static_cast<unsigned>(region->RelocateObserve()),
                     static_cast<unsigned>(region->IsForwardingDone()),
                     static_cast<unsigned>(region->GetRegionType()), region->GetLiveByteCount());
    }
}

void RegionManager::CollectFromSpaceGarbage()
{
    // cjpmnull2 5b31efeb mirrored onto this second reclaim entry: a page still
    // in the relocation set (route ∉ {FORWARDED,COMPACTED} and not Exempt-kept)
    // must not be merged into garbage. ZGC free_page never runs while the page
    // is in the relocation set (zGeneration.cpp:216-221).
    static std::atomic<size_t> g_fromGarbageSkip{ 0 };
    RegionInfo* region = fromRegionList.TakeHeadRegion();
    while (region != nullptr) {
        const bool complete = region->IsForwardingDone();
        if (!complete) {
            const size_t n = g_fromGarbageSkip.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][from-garbage-skip] n=%zu region=%p start=%#zx route=%u done=%u live=%zu "
                    "— skip CollectFromSpaceGarbage, Exempt",
                    n, region, region->GetRegionStart(), region->IsForwardingDone() ? 1u : 0u,
                    static_cast<unsigned>(region->IsForwardingDone()), region->GetLiveByteCount());
            }
            ExemptFromRegion(region);
        } else {
#if defined(__OHOS__)
            if (region->IsGhostFromRegion()) {
                garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
            } else {
                ReclaimRegion(region);
            }
#else
            garbageRegionList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
#endif
        }
        region = fromRegionList.TakeHeadRegion();
    }
}

template<Generation G>
void RegionManager::ForwardFromRegions()
{
    detail::ExecuteForwardTask<G>(*this, fromRegionList);

    VLOG(REPORT, "forward %zu from-region units", fromRegionList.GetUnitCount());

    AllocBuffer* allocBuffer = AllocBuffer::GetAllocBuffer();
    if (LIKELY(allocBuffer != nullptr)) {
        allocBuffer->ClearRegion(); // clear region for next GC
    }
}

size_t RegionManager::CollectFreePinnedSlots(RegionInfo* region)
{
    // pinroot: raw-pointer pin is a liveness hold — do not free any slot while count > 0.
    // AddRawPointerObject only bumps this counter (no mark bit / root set); reclaim must honour it.
    if (region->GetRawPointerObjectCount() > 0) {

        return 0;
    }
    // traverse pinned region to reclaim free pinned objects.
    size_t start = region->GetRegionStart();
    size_t garbageSize = 0;
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    region->VisitAllObjects([this, region, view, start, &garbageSize](BaseObject* object) {
        size_t offset = reinterpret_cast<MAddress>(object) - start;
        if (!region->IsSurvivedObject(view, offset)) {
            if (!Collector::PlausibleManagedObjectGate("CollectFreePinnedSlots", object)) {
                return;
            }
            size_t objSize = object->GetSize();
            DLOG(ALLOC, "reclaim pinned obj %p<%p>(%zu)", object, object->GetTypeInfo(), objSize);
            garbageSize += objSize;
            std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
            ReleaseNativeResource(object);
            freePinnedSlotLists.PushFront(object);
        }
    });
    return garbageSize;
}

size_t RegionManager::CollectPinnedGarbage()
{

    {
        std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
        freePinnedSlotLists.Clear();
    }
    size_t garbageSize = 0;
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        // pinroot: whole-region reclaim also ignores pins; skip while any raw pointer holds.
        if (region->GetRawPointerObjectCount() > 0) {

            region = region->GetNextRegion();
            continue;
        }
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        if (region->IsKnownEmpty(view)) {
            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldPinnedRegionList.DeleteRegion(del);

            auto fixToObj = [](BaseObject* obj) { ReleaseNativeResource(obj); };
            del->VisitAllObjects(fixToObj);


            garbageSize += CollectRegion<Generation::Old>(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
            region = region->GetNextRegion();
        }
    }

    return garbageSize;
}

size_t RegionManager::CollectLargeGarbage()
{
    size_t garbageSize = 0;
    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        // holdercapture: sample the face here, BEFORE the predicate below decides.
        //
        // Sampling early is necessary but NOT sufficient, and the earlier version of this
        // comment claimed otherwise. Through one view the two predicates are ordered, not
        // equal: for a large region IsMarkedObject(view,0) is GetMarkedRegionFlag(view)==1
        // while IsSurvivedObject(view,0) is that OR isResurrected, so marked implies
        // survived. Every region this loop releases failed !IsSurvivedObject(view,0) and
        // therefore reads marked==0 through that same view - one line earlier just as
        // surely as at the top of ReleaseRegion. Moving the sample moves the zero; it does
        // not remove it.
        //
        // The mark bit read through the view below is a control, not the finding: it must
        // be 0 on every released region, and if it ever is not, the reading of this
        // predicate is wrong and the rest of the measurement is void.

        // for large region, the offset of obj is 0
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        if (!region->IsSurvivedObject(view, 0)) {
            DLOG(REGION, "reclaim large region %p@[0x%zx+%zu, 0x%zx) type %u", region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldLargeRegionList.DeleteRegion(del);
            if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseRegion(del);
            } else {

                garbageSize += CollectRegion<Generation::Old>(del);
            }
        } else {
            region->ResetMarkBit(view);
            region = region->GetNextRegion();
        }
    }

    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
        region->ResetMarkBit(view);
        region = region->GetNextRegion();
    }

    return garbageSize;
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionManager::DumpRegionInfo() const
{
    if (!ENABLE_LOG(ALLOC)) {
        return;
    }
    VisitPageOwners([&](RegionInfo* region) {
        if (!region->IsFreeRegion()) {
            region->DumpRegionInfo(ALLOC);
        }
    });
}
#endif

void RegionManager::DumpRegionStats(const char* msg) const
{
    size_t totalSize = GetHeapCapacity();
    VLOG(REPORT, "heap backing capacity %zu bytes", GetCommittedCapacity());
    size_t totalUnits = totalSize / RegionInfo::UNIT_SIZE;
    size_t activeSize = GetActiveUnitCount() * RegionInfo::UNIT_SIZE;
    size_t activeUnits = activeSize / RegionInfo::UNIT_SIZE;

    size_t tlRegions = tlRegionList.GetRegionCount();
    size_t tlUnits = tlRegionList.GetUnitCount();
    size_t tlSize = tlUnits * RegionInfo::UNIT_SIZE;
    size_t allocTLSize = tlRegionList.GetAllocatedSize();

    size_t fromRegions = fromRegionList.GetRegionCount();
    size_t fromUnits = fromRegionList.GetUnitCount();
    size_t fromSize = fromUnits * RegionInfo::UNIT_SIZE;
    size_t allocFromSize = fromRegionList.GetAllocatedSize();

    size_t unmovableRegions = unmovableFromRegionList.GetRegionCount();
    size_t unmovableUnits = unmovableFromRegionList.GetUnitCount();
    size_t unmovableSize = unmovableUnits * RegionInfo::UNIT_SIZE;
    size_t allocUnmovableSize = unmovableFromRegionList.GetAllocatedSize();

    size_t keptRegions = 0;
    size_t keptUnits = 0;
    size_t keptSize = 0;
    size_t keptLive = 0;
    auto censusKept = [&keptRegions, &keptUnits, &keptSize, &keptLive](RegionInfo* region) {
        if (region == nullptr || !region->IsForwardingDone()) {
            return;
        }
        if (region->IsForwardingDone() && !region->IsCompacted()) {
            return;
        }
        ++keptRegions;
        keptUnits += region->GetUnitCount();
        keptSize += region->GetRegionSize();
        keptLive += region->GetLiveByteCount();
    };
    fromRegionList.VisitAllRegions(censusKept);
    unmovableFromRegionList.VisitAllRegions(censusKept);
    recentFullRegionList.VisitAllRegions(censusKept);

    size_t recentFullRegions = recentFullRegionList.GetRegionCount();
    size_t recentFullUnits = recentFullRegionList.GetUnitCount();
    size_t recentFullSize = recentFullUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentFullSize = recentFullRegionList.GetAllocatedSize();
    RecentFullAccounting::Report(recentFullRegions, recentFullSize);

    size_t garbageRegions = garbageRegionList.GetRegionCount();
    size_t garbageUnits = garbageRegionList.GetUnitCount();
    size_t garbageSize = garbageUnits * RegionInfo::UNIT_SIZE;
    size_t allocGarbageSize = garbageRegionList.GetAllocatedSize();

    size_t pinnedRegions = oldPinnedRegionList.GetRegionCount();
    size_t pinnedUnits = oldPinnedRegionList.GetUnitCount();
    size_t pinnedSize = pinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocPinnedSize = oldPinnedRegionList.GetAllocatedSize();

    size_t recentPinnedRegions = recentPinnedRegionList.GetRegionCount();
    size_t recentPinnedUnits = recentPinnedRegionList.GetUnitCount();
    size_t recentPinnedSize = recentPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentPinnedSize = recentPinnedRegionList.GetAllocatedSize();

    size_t rawPointerPinnedRegions = rawPointerPinnedRegionList.GetRegionCount();
    size_t rawPointerPinnedUnits = rawPointerPinnedRegionList.GetUnitCount();
    size_t rawPointerPinnedSize = rawPointerPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRawPointerPinnedSize = rawPointerPinnedRegionList.GetAllocatedSize();

    size_t largeRegions = oldLargeRegionList.GetRegionCount();
    size_t largeUnits = oldLargeRegionList.GetUnitCount();
    size_t largeSize = largeUnits * RegionInfo::UNIT_SIZE;
    size_t allocLargeSize = oldLargeRegionList.GetAllocatedSize();

    size_t recentlargeRegions = recentLargeRegionList.GetRegionCount();
    size_t recentlargeUnits = recentLargeRegionList.GetUnitCount();
    size_t recentLargeSize = recentlargeUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentLargeSize = recentLargeRegionList.GetAllocatedSize();

    size_t allHeapSize = GetHeapCapacity();
    size_t allUnits = allHeapSize / RegionInfo::UNIT_SIZE;
    size_t inactiveUnits = GetInactiveUnitCount();

    size_t usedUnitCount = GetUsedUnitCount();
    size_t usedObjSize = GetAllocatedSize();
    size_t releasedUnits = freeRegionManager.GetReleasedUnitCount();
    size_t dirtyUnits = freeRegionManager.GetDirtyUnitCount();
    size_t dirtySize = dirtyUnits * RegionInfo::UNIT_SIZE;

    size_t totalUnitCount = usedUnitCount + garbageUnits + dirtyUnits;
    size_t totalObjSize = usedObjSize + garbageSize + dirtyUnits * RegionInfo::UNIT_SIZE;

    double objectCapacity = (allHeapSize > 0) ? static_cast<double>(totalObjSize) / allHeapSize : 0.0;
    double unitCapacity = (allUnits > 0) ? static_cast<double>(totalUnitCount) / allUnits : 0.0;
    double usedObjectCapacity = (allHeapSize > 0) ? static_cast<double>(usedObjSize) / allHeapSize : 0.0;
    double usedUnitCapacity = (allUnits > 0) ? static_cast<double>(usedUnitCount) / allUnits : 0.0;
    double objFragRate = 1.0 - objectCapacity;
    double unitFragRate = 1.0 - unitCapacity;
    double usedObjFragRate = 1.0 - usedObjectCapacity;
    double usedUnitFragRate = 1.0 - usedUnitCapacity;

#define DUMP_REGION_STATS_LOG(format, ...) VLOG(REPORT, format, ##__VA_ARGS__)

    DUMP_REGION_STATS_LOG("%s", msg);

    DUMP_REGION_STATS_LOG("\ttotal units: %zu (%zu B)", totalUnits, totalSize);
    DUMP_REGION_STATS_LOG("\tactive units: %zu (%zu B)", activeUnits, activeSize);
    DUMP_REGION_STATS_LOG("\tinactive units: %zu (%zu B)", inactiveUnits, inactiveUnits * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\ttl-regions %zu: %zu units (%zu B, alloc %zu)", tlRegions,  tlUnits, tlSize, allocTLSize);
    DUMP_REGION_STATS_LOG("\tfrom-regions %zu: %zu units (%zu B, alloc %zu)", fromRegions,  fromUnits, fromSize,
                          allocFromSize);
    DUMP_REGION_STATS_LOG("\tunmovable-from regions %zu: %zu units (%zu B, alloc %zu)", unmovableRegions,
                          unmovableUnits, unmovableSize, allocUnmovableSize);
    DUMP_REGION_STATS_LOG("\tkept-publish regions %zu: %zu units (%zu B, live %zu, hole %zu)", keptRegions, keptUnits,
                          keptSize, keptLive, keptSize > keptLive ? keptSize - keptLive : 0);
    DUMP_REGION_STATS_LOG("\trecent-full regions %zu: %zu units (%zu B, alloc %zu)",
                          recentFullRegions, recentFullUnits, recentFullSize, allocRecentFullSize);
    DUMP_REGION_STATS_LOG("\tgarbage regions %zu: %zu units (%zu B, alloc %zu)",
                          garbageRegions, garbageUnits, garbageSize, allocGarbageSize);
    DUMP_REGION_STATS_LOG("\tpinned regions %zu: %zu units (%zu B, alloc %zu)",
                          pinnedRegions, pinnedUnits, pinnedSize, allocPinnedSize);
    DUMP_REGION_STATS_LOG("\trecent pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          recentPinnedRegions, recentPinnedUnits, recentPinnedSize, allocRecentPinnedSize);
    DUMP_REGION_STATS_LOG("\trawPointer pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          rawPointerPinnedRegions, rawPointerPinnedUnits, rawPointerPinnedSize,
                          allocRawPointerPinnedSize);
    DUMP_REGION_STATS_LOG("\tlarge-object regions %zu: %zu units (%zu B, alloc %zu)",
                          largeRegions, largeUnits, largeSize, allocLargeSize);
    DUMP_REGION_STATS_LOG("\trecent large-object regions %zu: %zu units (%zu B, alloc %zu)",
                          recentlargeRegions, recentlargeUnits, recentLargeSize, allocRecentLargeSize);
    DUMP_REGION_STATS_LOG("\tused summary: usedUnits %zu (%zu B), usedObjSize %zu B",
                          usedUnitCount, usedUnitCount * RegionInfo::UNIT_SIZE, usedObjSize);

    size_t releasedMaxBlock = freeRegionManager.GetReleasedMaxBlock();
    size_t dirtyMaxBlock = freeRegionManager.GetDirtyMaxBlock();
    size_t releasedNodeCount = freeRegionManager.GetReleasedNodeCount();
    size_t dirtyNodeCount = freeRegionManager.GetDirtyNodeCount();
    DUMP_REGION_STATS_LOG("\treleased units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          releasedUnits, releasedUnits * RegionInfo::UNIT_SIZE,
                          releasedNodeCount,
                          releasedMaxBlock, releasedMaxBlock * RegionInfo::UNIT_SIZE);
    DUMP_REGION_STATS_LOG("\tdirty units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          dirtyUnits, dirtyUnits * RegionInfo::UNIT_SIZE, dirtyNodeCount,
                          dirtyMaxBlock,
                          dirtyMaxBlock * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\tgarbage+dirty summary: garbageUnits %zu (%zu B, allocObj %zu), dirtyUnits %zu (%zu B)",
                          garbageUnits, garbageSize, allocGarbageSize, dirtyUnits, dirtySize);
    DUMP_REGION_STATS_LOG("\tobjectCapacity: %.4f (totalObjSize %zu / allHeapSize %zu), objFragRate: %.4f",
                          objectCapacity, totalObjSize, allHeapSize, objFragRate);
    DUMP_REGION_STATS_LOG("\tunitCapacity: %.4f (totalUnitCount %zu / allUnits %zu), unitFragRate: %.4f",
                          unitCapacity, totalUnitCount, allUnits, unitFragRate);
    DUMP_REGION_STATS_LOG("\tusedObjectCapacity: %.4f (usedObjSize %zu / allHeapSize %zu), usedObjFragRate: %.4f",
                          usedObjectCapacity, usedObjSize, allHeapSize, usedObjFragRate);
    DUMP_REGION_STATS_LOG("\tusedUnitCapacity: %.4f (usedUnitCount %zu / allUnits %zu), usedUnitFragRate: %.4f",
                          usedUnitCapacity, usedUnitCount, allUnits, usedUnitFragRate);
#undef DUMP_REGION_STATS_LOG

    TRACE_COUNT("CJRT_GC_totalSize", totalSize);
    TRACE_COUNT("CJRT_GC_totalUnits", totalUnits);
    TRACE_COUNT("CJRT_GC_activeSize", activeSize);
    TRACE_COUNT("CJRT_GC_activeUnits", activeUnits);
    TRACE_COUNT("CJRT_GC_tlRegions", tlRegions);
    TRACE_COUNT("CJRT_GC_tlUnits", tlUnits);
    TRACE_COUNT("CJRT_GC_tlSize", tlSize);
    TRACE_COUNT("CJRT_GC_allocTLSize", allocTLSize);
    TRACE_COUNT("CJRT_GC_fromRegions", fromRegions);
    TRACE_COUNT("CJRT_GC_fromUnits", fromUnits);
    TRACE_COUNT("CJRT_GC_fromSize", fromSize);
    TRACE_COUNT("CJRT_GC_allocFromSize", allocFromSize);
    TRACE_COUNT("CJRT_GC_recentFullRegions", recentFullRegions);
    TRACE_COUNT("CJRT_GC_recentFullUnits", recentFullUnits);
    TRACE_COUNT("CJRT_GC_recentFullSize", recentFullSize);
    TRACE_COUNT("CJRT_GC_allocRecentFullSize", allocRecentFullSize);
    TRACE_COUNT("CJRT_GC_garbageRegions", garbageRegions);
    TRACE_COUNT("CJRT_GC_garbageUnits", garbageUnits);
    TRACE_COUNT("CJRT_GC_garbageSize", garbageSize);
    TRACE_COUNT("CJRT_GC_allocGarbageSize", allocGarbageSize);
    TRACE_COUNT("CJRT_GC_pinnedRegions", pinnedRegions);
    TRACE_COUNT("CJRT_GC_pinnedUnits", pinnedUnits);
    TRACE_COUNT("CJRT_GC_pinnedSize", pinnedSize);
    TRACE_COUNT("CJRT_GC_allocPinnedSize", allocPinnedSize);
    TRACE_COUNT("CJRT_GC_recentPinnedRegions", recentPinnedRegions);
    TRACE_COUNT("CJRT_GC_recentPinnedUnits", recentPinnedUnits);
    TRACE_COUNT("CJRT_GC_recentPinnedSize", recentPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRecentPinnedSize", allocRecentPinnedSize);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedRegions", rawPointerPinnedRegions);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedUnits", rawPointerPinnedUnits);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedSize", rawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRawPointerPinnedSize", allocRawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_largeRegions", largeRegions);
    TRACE_COUNT("CJRT_GC_largeUnits", largeUnits);
    TRACE_COUNT("CJRT_GC_largeSize", largeSize);
    TRACE_COUNT("CJRT_GC_allocLargeSize", allocLargeSize);
    TRACE_COUNT("CJRT_GC_recentlargeRegions", recentlargeRegions);
    TRACE_COUNT("CJRT_GC_recentlargeUnits", recentlargeUnits);
    TRACE_COUNT("CJRT_GC_recentLargeSize", recentLargeSize);
    TRACE_COUNT("CJRT_GC_allocRecentLargeSize", allocRecentLargeSize);
    TRACE_COUNT("CJRT_GC_usedUnits", usedUnitCount);
    TRACE_COUNT("CJRT_GC_releasedUnits", releasedUnits);
    TRACE_COUNT("CJRT_GC_dirtyUnits", dirtyUnits);
    TRACE_COUNT("CJRT_GC_listedUnits", totalUnitCount);
    [[maybe_unused]] constexpr size_t decimalPrecision = 10000;
    TRACE_COUNT("CJRT_GC_objectCapacity", static_cast<size_t>(objectCapacity * decimalPrecision));
    TRACE_COUNT("CJRT_GC_unitCapacity", static_cast<size_t>(unitCapacity * decimalPrecision));
}

RegionInfo* RegionManager::AllocateThreadLocalRegion(size_t size, bool expectPhysicalMem, bool youngRegion,
                                                   bool allowSaferegion)
{
    // ZHeap::max_tlab_size / unsafe_max_tlab_alloc (zHeap.cpp:144-160):
    // the caller computes the refill size; the allocator enforces its extent.
    if (size == 0 || size > GetThreadLocalRegionSize()) {
        return nullptr;
    }
    const size_t units = AlignUp(size, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
    RegionInfo* region = TakeRegion(units, RegionInfo::UnitRole::SMALL_SIZED_UNITS, expectPhysicalMem,
                                    allowSaferegion);
    if (region != nullptr) {
        {
            region->SetYoungRegionFlag(youngRegion ? 1 : 0);
            if (youngRegion) {
                // zHeap.cpp:233: charge the backing extent even before a
                // prepared region is installed as a thread's current TLAB.
                tlabUsed.fetch_add(region->GetRegionSize(), std::memory_order_relaxed);
            }
            region->SetYoungAge(0);
            GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
            }
            // twoflags: POST_TRACE+ only (TRACE uses isTraceRegion). No CLEAR_SATB.
            if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
                phase == GC_PHASE_FORWARD) {
                region->SetNotRelocatableThisCycle(1);
            }
            tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
            DLOG(REGION, "alloc tl-region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu, %zu) type %u",
                region, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(),
                region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
                region->GetRegionType());
        }
    }

    return region;
}

// ZHeap::undo_alloc_page (zHeap.cpp:270): only a failed publication of
// a newly allocated, unused backing region cancels its allocation charge.
// GC retirement/reclamation is not an undo and must retain that cycle's usage.
void RegionManager::UndoThreadLocalRegionAllocation(RegionInfo* region)
{
    CHECK(region != nullptr && region->IsEmpty() && region->IsThreadLocalRegion());
    if (region->IsYoungRegion()) {
        const size_t size = region->GetRegionSize();
        const size_t previous = tlabUsed.fetch_sub(size, std::memory_order_relaxed);
        CHECK(previous >= size);
    }
    RemoveThreadLocalRegion(region);
    ReclaimRegion(region);
}

void RegionManager::RequestForRegion(size_t size)
{
    if (IsGcThread()) {
        // gc thread is always permitted for allocation.
        return;
    }

    Heap& heap = Heap::GetHeap();
    GCStats& gcstats = heap.GetCollector().GetGCStats();
    size_t allocatedBytes = GetAllocatedSize() - gcstats.liveBytesAfterGC;
    constexpr double pi = 3.14;
    size_t availableBytesAfterGC = heap.GetMaxCapacity() - gcstats.liveBytesAfterGC;
    double heuAllocRate = std::cos((pi / 2.0) * allocatedBytes / availableBytesAfterGC) * gcstats.collectionRate;
    // for maximum performance, choose the larger one.
    double allocRate = std::max(
        static_cast<double>(CangjieRuntime::GetHeapParam().allocationRate) * MB / SECOND_TO_NANO_SECOND, heuAllocRate);
    size_t waitTime = static_cast<size_t>(size / allocRate);
    uint64_t now = TimeUtil::NanoSeconds();
    if (prevRegionAllocTime + waitTime <= now) {
        prevRegionAllocTime = TimeUtil::NanoSeconds();
        return;
    }

    uint64_t sleepTime = std::min<uint64_t>(CangjieRuntime::GetHeapParam().allocationWaitTime,
                                  prevRegionAllocTime + waitTime - now);
    DLOG(ALLOC, "wait %zu ns to alloc %zu(B)", sleepTime, size);
    std::this_thread::sleep_for(std::chrono::nanoseconds{ sleepTime });
    prevRegionAllocTime = TimeUtil::NanoSeconds();
}

bool RegionManager::RelocateClaimedPage(RegionInfo* region)
{
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");
    MAddress regionStart = region->GetRegionStart();
    MAddress regionLimit = region->GetRegionAllocPtr();
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    bool allocFailed = false;
    ForEachLiveObjectStart(region, regionStart, regionLimit, [&](BaseObject* currentObj, size_t) {
        if (allocFailed) {
            return;
        }
        if (ForwardingTable::LookupForwarding(reinterpret_cast<MAddress>(currentObj),
                ForwardingTable::RetainPageOwner(region).get()).to) {
            return;
        }
        if (collector.ForwardObjectExclusive(currentObj) == nullptr) {
            allocFailed = true;
        }
    });
    if (allocFailed) {
        CompactRegion(region);
        return false;
    }
    VerifyForwardingReceiptsClosed(region, "RelocateClaimedPage");
    return true;
}

void RegionManager::CompactRegion(RegionInfo* region)
{
    auto owner = ForwardingTable::RetainPageOwner(region);
    ZForwardingLife::PageWorkScope work(owner.get(),
        owner && ZForwardingLife::CurrentPageWork() != owner.get());
    if (owner && owner->ref_count().load(std::memory_order_acquire) > 0) {
        owner->in_place_relocation_claim_page();
    }
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(9, region, nullptr);
#endif

    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u", region, regionStart,
        region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
    MAddress regionLimit = region->GetRegionAllocPtr();
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, regionStart);
    CHECK_DETAIL(static_cast<bool>(publication),
                 "compact forwarding table unavailable before copy region=%p range=[%#zx,%#zx)",
                 region, static_cast<size_t>(regionStart), static_cast<size_t>(region->GetRegionEnd()));
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    region->FreeCompactRouteTable();
    region->EnsureCompactRouteTable();
    region->SetRegionAllocPtr(regionStart);
    // ZGC zRelocate.cpp:838-861 start_in_place_relocation_prepare_remset: this page is its own
    // to-page, so its old remembered-set bits have to leave the face before the copy walk starts
    // writing the new ones.  What the walk does not hand back is dropped, which is
    // clear_remset_before_in_place_reuse (zRelocate.cpp:1027-1035).
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    std::vector<RememberedSet::InPlaceSlot> takenSlots;
    rememberedSet.TakeInPlaceSlots(regionStart, region->GetRegionEnd(), takenSlots);
    ForEachLiveObjectStart(region, regionStart, regionLimit, [&](BaseObject* currentObj, size_t offset) {
        const MAddress currentPtr = regionStart + offset;
        if (ForwardingTable::LookupForwarding(currentPtr, ForwardingTable::RetainPageOwner(region).get()).to) {
            return;
        }
        size_t size = currentObj->GetSize();
        MAddress toAddress = region->Alloc(size);
        BaseObject* toObj = from_region_addr(toAddress);
        DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
        collector.CopyObject(*currentObj, *toObj, size);
        toObj->SetStateCode(ObjectState::NORMAL);
        std::atomic_thread_fence(std::memory_order_release);
        const MAddress receipt = ForwardingTable::InsertMapping(publication, currentPtr, toAddress);

        region->RecordCompactRoute(offset, toAddress);
        // ZGC zRelocate.cpp:652-731 update_remset_old_to_old: the bits covering the from copy
        // name field offsets inside this object, so they follow it to its new address.
        rememberedSet.MoveInPlaceSlots(takenSlots, currentPtr, toAddress, size);
    });

    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            FillerZeroDiag::Note(FillerZeroDiag::Site::COMPACT, cur, reclaimSize);
            HeapFiller::ZeroAndFill(cur, reclaimSize);
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();
    VerifyForwardingReceiptsClosed(region, "CompactRegion.whole");
    WaitCopiedObjectsUnlocked(region);
    region->MarkForwardingDone();
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(11, region, nullptr);
#endif

    // zForwarding.cpp:171-181 / zRelocate.cpp:1001-1047: the forwarding table
    // outlives page reuse. Do not put this page on the mutator TLAB list while
    // its table is live — RehomeCompactedInPlaceRegion keeps it collector-visible.
    RehomeCompactedInPlaceRegion(region);
}

void RegionManager::EnlistCompactedRegionForAllocator(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    const RegionInfo::RegionType type = region->GetRegionType();
    bool claimed = false;
    if (type == RegionInfo::RegionType::FROM_REGION) {
        claimed = fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                                 RegionInfo::RegionType::THREAD_LOCAL_REGION);
    } else if (type == RegionInfo::RegionType::LONE_FROM_REGION) {
        region->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
        claimed = true;
    } else if (type == RegionInfo::RegionType::GARBAGE_REGION) {
        claimed = garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                                    RegionInfo::RegionType::THREAD_LOCAL_REGION);
    } else if (type == RegionInfo::RegionType::THREAD_LOCAL_REGION ||
               type == RegionInfo::RegionType::RECENT_FULL_REGION) {
        // Already owned by the allocator list, or the concurrent stay-young
        // path won and made it collector-visible. Both are complete states.
        return;
    }
    if (claimed) {
        tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
}

// A region the forward path finished with in place has to stay reachable by a collection-set
// builder, and CompactRegion leaves it on tlRegionList, which no builder walks.
//
// ZGC gets this structurally: a page is in _page_table from ZHeap::alloc_page (zHeap.cpp:257) until
// ZHeap::free_page (:277), and select_relocation_set iterates that table
// (zGeneration.cpp:205-212), so allocator ownership and collection visibility are separate
// questions. Ours ties them together through a list, and the compact-in-place arm drops the
// allocator side without moving the region: AllocBuffer::ClearRegion (AllocBuffer.h:36-44) only
// nulls tlRegion, it does not unlink anything.
//
// The result is a region no path can reach again. It cannot be allocated from --
// AllocateThreadLocalRegion always takes a fresh region -- and it cannot be collected, because
// AssembleSmallGarbageCandidates and PrepareYoungGarbageCandidates walk fromRegionList,
// recentFullRegionList and unmovableFromRegionList, and neither walks tlRegionList. It is simply
// retained until the heap goes away.
//
// Same shape as the stay-young survivor that had to be re-homed earlier in this cycle: the work
// finished, and nothing put the region back where the next cycle looks.
void RegionManager::RehomeCompactedInPlaceRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    const RegionInfo::RegionType type = region->GetRegionType();
    bool claimed = false;
    if (type == RegionInfo::RegionType::FROM_REGION) {
        claimed = fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                                 RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::LONE_FROM_REGION) {
        region->SetRegionType(RegionInfo::RegionType::RECENT_FULL_REGION);
        claimed = true;
    } else if (type == RegionInfo::RegionType::GARBAGE_REGION) {
        claimed = garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                                    RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::THREAD_LOCAL_REGION) {
        claimed = tlRegionList.TryDeleteRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION,
                                               RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::RECENT_FULL_REGION) {
        return;
    }
    if (!claimed) {
        return;
    }
    recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}

void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)
{
    auto owner = ForwardingTable::RetainPageOwner(region);
    ZForwardingLife::PageWorkScope work(owner.get(),
        owner && ZForwardingLife::CurrentPageWork() != owner.get());
    if (owner && owner->ref_count().load(std::memory_order_acquire) > 0) {
        owner->in_place_relocation_claim_page();
    }
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(9, region, nullptr);
#endif

    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u to region %p@%#zx:%#zx",
        region, regionStart, region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType(),
        toRegion1, toRegion1->GetRegionStart(), toRegion1->GetRegionAllocPtr());
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, regionStart);
    CHECK_DETAIL(static_cast<bool>(publication),
                 "partial compact forwarding table unavailable before copy region=%p range=[%#zx,%#zx)",
                 region, static_cast<size_t>(regionStart), static_cast<size_t>(region->GetRegionEnd()));
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    MAddress regionLimit = region->GetRegionAllocPtr();
    region->FreeCompactRouteTable();
    region->EnsureCompactRouteTable();
    region->SetRegionAllocPtr(regionStart);
    // zRelocate.cpp:838-861, as in the whole-page arm above.
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    std::vector<RememberedSet::InPlaceSlot> takenSlots;
    rememberedSet.TakeInPlaceSlots(regionStart, region->GetRegionEnd(), takenSlots);
    ForEachLiveObjectStart(region, regionStart, regionLimit, [&](BaseObject* currentObj, size_t offset) {
        const MAddress currentPtr = regionStart + offset;
        if (ForwardingTable::LookupForwarding(currentPtr, ForwardingTable::RetainPageOwner(region).get()).to) {
            return;
        }
        size_t size = currentObj->GetSize();
        MAddress toAddress = toRegion1->Alloc(size);
        if (toAddress == 0) {
            toAddress = region->Alloc(size);
        }
        BaseObject* toObj = from_region_addr(toAddress);
        DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
        collector.CopyObject(*currentObj, *toObj, size);
        toObj->SetStateCode(ObjectState::NORMAL);
        std::atomic_thread_fence(std::memory_order_release);
        const MAddress receipt = ForwardingTable::InsertMapping(publication, currentPtr, toAddress);

        region->RecordCompactRoute(offset, toAddress);
        // zRelocate.cpp:652-731, as in the whole-page arm above.  toAddress may be in toRegion1,
        // which is what ZGC means by "even with in-place relocation, the to_page could be another
        // page" (zRelocate.cpp:666-667).
        rememberedSet.MoveInPlaceSlots(takenSlots, currentPtr, toAddress, size);
    });

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact_partial", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            FillerZeroDiag::Note(FillerZeroDiag::Site::COMPACT_PARTIAL, cur, reclaimSize);
            HeapFiller::ZeroAndFill(cur, reclaimSize);
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact_partial memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();
    VerifyForwardingReceiptsClosed(region, "CompactRegion.partial");
    WaitCopiedObjectsUnlocked(region);
    region->MarkForwardingDone();
#if defined(MRT_TESTABLE_INTERNALS)
    RunRemapWindowTestHook(11, region, nullptr);
#endif

    RehomeCompactedInPlaceRegion(region);
}

namespace {
bool StayYoungThisCycle(RegionInfo* region)
{
    if (!kPageAgeAdaptiveTenuring) {
        return false;
    }
    const uint32_t thr = Heap::GetHeap().GetCollector().GetGCStats().tenuringThreshold;
    return !ShouldPromoteAge(region->GetYoungAge(), thr);
}

} // namespace

void RegionManager::BumpYoungSurvivorAge(RegionInfo* region)
{
    uint8_t next = region->GetYoungAge();
    if (next < untype(PageAge::survivor14)) {
        region->SetYoungAge(static_cast<uint8_t>(next + 1));
    }
}

void RegionManager::FinishStayYoungInPlace(RegionInfo* region, bool advanceAge)
{
    if (advanceAge) {
        BumpYoungSurvivorAge(region);
    }
    WaitCopiedObjectsUnlocked(region);
    VerifyForwardingReceiptsClosed(region, "FinishStayYoungInPlace");
#if defined(MRT_TESTABLE_INTERNALS)
    // zRelocate.cpp:1137-1153: the real producer has published its receipts;
    // pause before done so a waiting mutator can consume that exact state.
    RunRemapWindowTestHook(3, region, nullptr);
#endif
    region->MarkForwardingDone();
    // The selected-set carrier remains queryable after payload release.
    // The next selection/reset retires its ghost/source view; completing this
    // page task does not revoke forwarding-table membership.
}

void RegionManager::EnlistStayYoungSurvivor(RegionInfo* region, bool advanceAge)
{
    FinishStayYoungInPlace(region, advanceAge);
    // evac_finish calls this on FROM regions still linked in fromRegionList.
    // PrependRegion overwrites next/prev without unlinking — later
    // CollectFromSpaceGarbage MergeRegionList walks a chain that now points
    // into recentFull, and DeleteRegionLocked SEGVs (r13=0, +0x14).
    const RegionInfo::RegionType type = region->GetRegionType();
    bool claimed = false;
    if (type == RegionInfo::RegionType::FROM_REGION) {
        claimed = fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                                 RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::LONE_FROM_REGION) {
        // TakeHeadRegion already unlinked it (RegionManager.cpp:1712). Type still
        // LONE_FROM until Prepend; kLoneFromIsFrom readers would keep treating it
        // as from-space if we skipped the store (WCollector.h:495).
        region->SetRegionType(RegionInfo::RegionType::RECENT_FULL_REGION);
        claimed = true;
    } else if (type == RegionInfo::RegionType::GARBAGE_REGION) {
        claimed = garbageRegionList.TryDeleteRegion(region, RegionInfo::RegionType::GARBAGE_REGION,
                                                    RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::THREAD_LOCAL_REGION) {
        // CompactRegion's ownership tail may win first. Transfer that completed
        // allocator-list state instead of either abandoning the survivor there
        // or linking the node into two lists.
        claimed = tlRegionList.TryDeleteRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION,
                                               RegionInfo::RegionType::RECENT_FULL_REGION);
    } else if (type == RegionInfo::RegionType::RECENT_FULL_REGION) {
        // RouteRegion's compact-in-place fallback already re-homed this region.
        // A second Prepend while it is the list head sets both links to itself.
        return;
    }
    if (!claimed) {
        return;
    }
    recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}

template<Generation G>
void RegionManager::ForwardRegion(RegionInfo* region)
{
    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", region->GetRegionType());

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %zu",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        region->GetRegionType(), region->GetLiveByteCount());

    bool youngRegion = region->IsYoungRegion();
    if (youngRegion && !GenerationMayRelocateYoung(G)) {
        // The old generation has no authority to interpret a young page's
        // liveness or promote it.  Keep it for the young generation without
        // advancing survivor age (zGeneration.cpp:195-221).
        EnlistStayYoungSurvivor(region, false);
        return;
    }
    MarkView<G> markView = region->GetRouteMarkView<G>();
    // oracleblack: the generational contract also guards this arm. The OLD pass stamps a
    // current-epoch mark face on young regions it never actually examines, so
    // "markedThisCycle ∧ live==0" holds vacuously for them and the residual f3-livehole
    // census (~128/run after the unmarked-arm gate below) was fed from here. Only the
    // YOUNG pass may prove a young region empty (zGeneration.cpp:216-221: each generation
    // frees only pages its own mark examined).
    if (IsKnownEmptyForView(region, markView) && !(youngRegion && G == Generation::Old)) {
        // cjpmnull2: IsKnownEmpty is now ZGC-shaped (this-cycle marked ∧ live==0).
        // Only those pages are empty; collect them (zGeneration.cpp:216-221).
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            (void)region->PromoteYoungRegion(promotionView);
        }

        CollectRegion<G>(region);
        return;
    }
    // Unmarked this cycle is not empty (zPage.inline.hpp:223-225). Still do
    // not keep every never-examined from-page: hangfloor showed young
    // neverExamined × Collect-skip fills the heap (10/10 HANG). Keep only
    // the two cjpmnull classes — residual live bytes, or a published plan
    // that has not been copied (route=3). live==0 FORWARDABLE is true dead.
    {
        const bool incompleteRoute = region->IsRoutingState() && !region->IsForwardingDone();
        const bool liveResidual = region->GetLiveByteCount() > 0;
        // hangfloor: young neverExamined×keep fills the heap. Old from-pages
        // with payload are the 59-class (route=1 liveinfo_null, live-slots>0).
        // live==0 after THIS cycle's mark is freed at ExemptFromRegions
        // (zGeneration.cpp:216-221), before the page is FORWARDABLE. Do not
        // Collect here: VisitLive copies nothing then FORWARDED+Collect is
        // the NW 256MB keep-from UAF (pc=0x8aa8 reclaim_satb).
        //
        // oracleblack: generational contract on the young arm. A young region's liveness is
        // the MINOR's to judge -- a minor marks young via remset+roots, so "no mark bitmap"
        // after a minor really means empty and the collect below is legitimate. A MAJOR
        // never examines young objects at all: under a workload whose config never fires a
        // minor (cjpm at 12GB: youngRegionTriggerBytes=32MB unreached inside the crash
        // window, cycles are HEU-only), every young region is permanently bitmap-less and
        // the old arm collected them wholesale while marked old holders still referenced
        // their objects (f3-livehole census: 64-512/run, reason=region_free, from==latest,
        // targets clustered per region). ZGC: a page is freed only by the generation that
        // proved it empty (zPage.inline.hpp:223-225 seqnum, zGeneration.cpp:216-221).
        // Keep unexamined young in the OLD pass; the YOUNG pass keeps its collect right,
        // so the hangfloor regression (young garbage never reclaimed) cannot return.
        if (region->GetMarkBitmap(markView) == nullptr &&
            region->GetRegionAllocPtr() > region->GetRegionStart() &&
            (incompleteRoute || liveResidual || !youngRegion || G == Generation::Old)) {
        static std::atomic<size_t> g_fwdUnmarkedKeep{ 0 };
        size_t n = g_fwdUnmarkedKeep.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][fwd-unmarked-keep] n=%zu region=%p start=%#zx alloc=%#zx "
                "route=%u live=%zu — ExemptFromRegion (not marked this cycle)",
                n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                static_cast<unsigned>(region->RelocateObserve()), region->GetLiveByteCount());
        }
        if (youngRegion && StayYoungThisCycle(region)) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            region->PreserveRetainedLiveInfo();
            {
                GCReason r = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const bool deferred = PromotedRegionDomain::DeferPromotedFieldScan(r == GC_REASON_YOUNG);
                if (deferred) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::Abandon);
                }
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 1, deferred);
                size_t recEdges = deferred ? 0 : RecordPromotedCrossGenEdges(region);
                PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(r), /*site*/ 1, recEdges);
            }
            (void)region->PromoteYoungRegion(promotionView);
        }
        ExemptFromRegion(region);
        region->DispelGhostFromRegion();
        return;
        }
    }

    const bool stayYoung = youngRegion && StayYoungThisCycle(region);
    if (stayYoung || !RouteRegion(region)) {
        if (youngRegion && stayYoung) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        // In-place relocation that copied nothing leaves the alloc pointer back at the
        // region start, so the size-walk over [start, start) is empty (RegionManager.cpp:
        // 665-668): the page holds no object to promote, no field to record an edge for and
        // nothing for a later discharge to walk.  ZGC reclaims such a page rather than
        // promoting it -- select_relocation_set hands every relocatable page its own mark
        // did not mark to register_empty_page and free_empty_pages frees it in bulk
        // (zGeneration.cpp:216-221 / 169-176).  And ZGC never routes an in-place relocated
        // from page into the flip-promoted remset walk at all: ZRelocateAddRemsetForFlipPromoted
        // is constructed over flip_promoted_pages() only (zRelocate.cpp:1304), the pages
        // promoted *without* relocation (ZFlipAgePagesTask, zRelocate.cpp:1334-1363).  The
        // in-place relocated from page goes on _in_place_relocate_promoted_pages
        // (zRelocate.cpp:896), and that array is read exactly once -- by
        // ZRelocationSet::reset's destroy_and_clear (zRelocationSet.cpp:200).
        if (region->GetRegionAllocPtr() <= region->GetRegionStart() &&
            !(youngRegion && G == Generation::Old)) {
            // ZGC frees such a page: select_relocation_set hands every relocatable page its
            // own mark did not mark to register_empty_page, and free_empty_pages returns it
            // to the page cache (zGeneration.cpp:216-221 / 169-176).  Keeping it homed on
            // recentFullRegionList instead leaves a *young* region whose alloc pointer has
            // been rewound to its start, and that has two measured consequences:
            //   - every stale pointer into its former contents still answers "my target is
            //     young" to the cross-gen edge walks, so a dead old object's field is
            //     replayed into the remembered set and then refused at ResolveStoreValue
            //     (NW256/256MB 3/3: slot=holder+0x2910 in an old RECENT_FULL region with
            //     holderSurvived=0 holderMarked=0, target=page+0xd100 with allocOff=0);
            //   - the page is re-selected as an empty relocation candidate every cycle --
            //     the same page re-entered this arm a cycle later with entryAllocOff=0.
            // RehomeCompactedInPlaceRegion (RegionManager.cpp:3766) put it on
            // recentFullRegionList, which is why CollectRegion's PrependRegion refused it
            // (GetRegionListOwner() == nullptr).  Take it back off that list first; the
            // ordinary empty-page arm above reaches CollectRegion the same way.
            if (region->GetRegionListOwner() == &recentFullRegionList) {
                const size_t units = region->GetUnitCount();
                recentFullRegionList.DeleteRegion(region);
                RecentFullAccounting::Dequeue(1, units);
            }
            CollectRegion<G>(region);
            return;
        }
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            // ZRelocate::relocate registers flip-promoted pages and defers their
            // field walk until after relocation (zRelocate.cpp:1289-1306).
            region->PreserveRetainedLiveInfo();
            {
                GCReason r = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const bool deferred = PromotedRegionDomain::DeferPromotedFieldScan(r == GC_REASON_YOUNG);
                if (deferred) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::InPlace);
                }
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 0, deferred);
                size_t recEdges = deferred ? 0 : RecordPromotedCrossGenEdges(region);
                PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(r), /*site*/ 0, recEdges);
            }
            (void)region->PromoteYoungRegion(promotionView);
        }
        return;
    }

    int32_t rawPointerCount = region->GetRawPointerObjectCount();
    CHECK(rawPointerCount == 0);
    Collector& collector = Heap::GetHeap().GetCollector();
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t promotedRecords = 0;
    size_t oldObjForwarded = 0;
    size_t o2yOnToForOld = 0;
    size_t recordedOnToForOld = 0;
    bool forwarded = region->VisitLiveObjectsUntilFalse(
        [&collector, region, youngRegion, &rememberedSet, &promotedRecords, &oldObjForwarded,
         &o2yOnToForOld, &recordedOnToForOld](BaseObject* obj) {
            BaseObject* toObj = collector.ForwardObject(obj,
                static_cast<Generation>(ForwardingTable::RetainPageOwner(region)->table_generation()));
            // Remset slots must address the surviving (to-space) holder, not the from copy
            // that CollectRegion is about to reclaim.
            //
            // Young: re-scan to-object and Record O→Y slots (promotion).
            // Old→old: ZGC update_remset_old_to_old — TransferObjectSlots moves existing
            // remset bits by field offset (zRelocate.cpp:652-731). From bits are scrubbed
            // later by CollectRegion → ClearRegion (no in-place overlap on this path:
            // RouteObject always mints a distinct to address).

            if (youngRegion && toObj != nullptr) {
                if (!Collector::PlausibleManagedObjectGate("ForwardRegion.to", toObj)) {
                    NoteFwdToGateRefuse("young", toObj);
                } else if (toObj->HasRefField()) {
                toObj->ForEachRefField([&rememberedSet, &promotedRecords, toObj, &collector](RefField<>& field) {
                    BaseObject* target = ScanFieldHealedTarget(collector, field);
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        rememberedSet.Record(slot);
                        ++promotedRecords;
                    }
                });
                }
            } else if (!youngRegion && toObj != nullptr && toObj != obj && obj->IsForwarded()) {
                if (!Collector::PlausibleManagedObjectGate("ForwardRegion.to", toObj)) {
                    NoteFwdToGateRefuse("old", toObj);
                } else {
                size_t sz = RegionSpace::GetAllocSize(*obj);
                MAddress fromBase = reinterpret_cast<MAddress>(obj);
                MAddress toBase = reinterpret_cast<MAddress>(toObj);
                ZForwarding* forwarding = ForwardingTable::GetCovering(fromBase, Generation::Old);
                const bool youngMarking = Heap::GetHeap().GetGCPhase() == GCPhase::GC_PHASE_TRACE;
                size_t moved = rememberedSet.TransferObjectSlots(fromBase, toBase, sz, forwarding, youngMarking);
                recordedOnToForOld += moved;

                }
            }
            // tipnull arm R: receipt = object FORWARDED (Copy wrote tip), not soft-keep from.
            return obj->IsForwarded();
        });
    if (!youngRegion) {
        if (ZForwarding* forwarding = ForwardingTable::GetCovering(region->GetRegionStart(), Generation::Old)) {
            forwarding->relocated_remembered_fields_after_relocate();
        }
    }

    // tipnull v5 full coverage: FORWARDED only if every liveInfo0 *live bit* is covered by
    // a size-walk start that is object-FORWARDED (Copy wrote tip). Prior allSurvivorsForwarded
    // only checked size-walk starts; multi-bit MarkBits interiors/orphans still Admitted
    // without ever being Copy'd → region_FORWARDED_tip_null (arm R refuse=0).
    // Incomplete: DispelGhost (no geometric plan) + Exempt — never FORWARDED empty, never
    // ROUTED forever (TIMEOUT), never soft-null after Collect (SEGV si_addr=0x8).
    auto allLiveBitsHaveReceipt = [region]() -> bool {
        auto survivedAt = [region](size_t offset) -> bool { return region->IsOwnerSurvivedObject(offset); };
        if (region->IsLargeRegion()) {
            if (!survivedAt(0)) {
                return true;
            }
            BaseObject* o = from_region_addr(region->GetRegionStart());
            if (!Collector::PlausibleManagedObjectGate("ForwardRegion-complete", o)) {
                return false;
            }
            return o->IsForwarded();
        }
        if (!region->IsSmallRegion()) {
            return true;
        }
        uintptr_t regionStart = region->GetRegionStart();
        uintptr_t allocPtr = region->GetRegionAllocPtr();
        size_t regionBytes = allocPtr > regionStart ? (allocPtr - regionStart) : 0;
        // Pass 1: size-walk starts that are survived must be object-FORWARDED.
        uintptr_t position = regionStart;
        while (position < allocPtr) {
            BaseObject* o = from_region_addr(position);
            size_t offset = position - regionStart;
            if (!Collector::PlausibleManagedObjectGate("ForwardRegion-complete", o)) {
                // Cannot walk further; any later survived bit is uncovered → fail.
                for (size_t rest = offset; rest < regionBytes; rest += kMarkedBytesPerBit) {
                    if (survivedAt(rest)) {
                        return false;
                    }
                }
                return true;
            }
            size_t allocSize = RegionSpace::GetAllocSize(*o);
            if (allocSize == 0) {
                return false;
            }
            if (survivedAt(offset) && !o->IsForwarded()) {
                return false;
            }
            position += allocSize;
        }
        // Pass 2: every survived 8B bit must lie in some size-walk object whose start
        // is object-FORWARDED (covers multi-bit interiors of densify MarkBits ranges).
        // Orphans (survived bit not inside any size-walk object) ⇒ fail.
        position = regionStart;
        size_t walkOff = 0;
        while (position < allocPtr) {
            BaseObject* o = from_region_addr(position);
            if (!Collector::PlausibleManagedObjectGate("ForwardRegion-cover", o)) {
                break;
            }
            size_t allocSize = RegionSpace::GetAllocSize(*o);
            if (allocSize == 0) {
                break;
            }
            bool startFwd = o->IsForwarded();
            for (size_t d = 0; d < allocSize; d += kMarkedBytesPerBit) {
                size_t bitOff = walkOff + d;
                if (survivedAt(bitOff) && !startFwd) {
                    return false;
                }
            }
            position += allocSize;
            walkOff += allocSize;
        }
        // Bits past last walkable object must not be survived (orphans / unwalkable tail).
        for (size_t rest = walkOff; rest < regionBytes; rest += kMarkedBytesPerBit) {
            if (survivedAt(rest)) {
                return false;
            }
        }
        return true;
    };

    if (!forwarded || !allLiveBitsHaveReceipt()) {
        forwarded = region->VisitLiveObjectsUntilFalse([&collector, region](BaseObject* obj) {
            if (obj->IsForwarded()) {
                return true;
            }
            (void)collector.ForwardObject(obj,
                static_cast<Generation>(ForwardingTable::RetainPageOwner(region)->table_generation()));
            return obj->IsForwarded();
        });
    }

    if (!forwarded || !allLiveBitsHaveReceipt()) {
        if (youngRegion && StayYoungThisCycle(region)) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            // Flip-promoted pages are registered here and walked once by the
            // post-relocation discharge (zRelocate.cpp:1289-1306).
            region->PreserveRetainedLiveInfo();
            {
                GCReason r = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const bool deferred = PromotedRegionDomain::DeferPromotedFieldScan(r == GC_REASON_YOUNG);
                if (deferred) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::Abandon);
                }
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 1, deferred);
                size_t recEdges = deferred ? 0 : RecordPromotedCrossGenEdges(region);
                PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(r), /*site*/ 1, recEdges);
            }
            (void)region->PromoteYoungRegion(promotionView);
        }
        // Complete the ZGC in-place shape first: Exempt publishes an identity
        // receipt for every kept object, then Dispel may retire the route.
        ExemptFromRegion(region);
        region->DispelGhostFromRegion();
        return;
    }
    {
        // zRelocate.cpp:1137-1152: the page worker finishes objects then
        // mark_done last (ForwardClaimedPage). Do not wait for own done here.
        // zRelocate.cpp:1152 — last act after every object on the page is relocated.
        VerifyForwardingReceiptsClosed(region, "ForwardRegion");
        region->MarkForwardingDone();
        // livesame ORDER + ZGC reset_livemap (zForwarding.cpp:71-74): one publish for
        // live bytes + mark face (ResetLiveMapAfterForward).
        {
            const uint64_t liveBefore = region->GetLiveByteCount();
            size_t validBefore = 0;
            size_t markedBefore = 0;

            region->VerifyLiveBooks(markView, "pre-ResetLiveMapAfterForward");
            // Simulated split for ORDER: live-only then mark-only was the old bug;
            // measure residual marks after live-zero before joint reset.
            region->ResetLiveByteCount();
            const uint64_t liveAfterReset = region->GetLiveByteCount();
            size_t validAfterReset = 0;
            size_t markedAfterReset = 0;

            // Joint publish (restores live empty + epoch bump in one API).
            region->ResetLiveMapAfterForward(markView);
            size_t validAfterInv = 0;
            size_t markedAfterInv = 0;


            region->VerifyLiveBooks(markView, "post-ResetLiveMapAfterForward");
            if (youngRegion) {
                if (promotedRecords != 0) {
                    g_promotedCrossGenEdgeCount.fetch_add(promotedRecords, std::memory_order_relaxed);
                }
                MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
                (void)region->PromoteYoungRegion(promotionView);
            }
            (void)validBefore;
            (void)validAfterReset;
            (void)validAfterInv;
        }
        // After-copy Collect zeros the from payload while live holders still name
        // it. ZGC free_page waits for detach (zRelocate.cpp:1041-1047) and keeps
        // the forwarding table until the next cycle (zRelocationSet.cpp:91-96).
        // cjpm coll_live: first young (cgen=0 fpath=2), then after that Exempt
        // old (cgen=1 fpath=2 route=5 ke=0 gh=1 reason=HEU). Exempt both; the
        // next Assemble/PrepareYoung re-enlists, ghost + entries stay until
        // PrepareFromRegionList. Residuals are settled above before done.
        ExemptFromRegion(region);
        return;
    }
}

uintptr_t RegionManager::AllocPinnedFromFreeList(size_t size)
{
    std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    // For preventing missing mark, do not allocate object from slot list when gc phase is post trace.
    if (mutatorPhase == GCPhase::GC_PHASE_POST_TRACE) {
        return 0;
    }
    uintptr_t allocPtr = freePinnedSlotLists.PopFront(size);
    if (allocPtr != 0) {
        M0Correlation::InvalidateStampBinding(allocPtr, M0Correlation::BindingInvalidation::PINNED_SLOT_REUSE);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(allocPtr);
        region->ResetCensusBoundary();
        region->PreserveRetainedLiveInfoUpTo(region->GetRegionStart());
    }
    // For making bitmap comform with live object count, do not mark object repeated.
    bool barrierClosedMarking = mutatorPhase == GCPhase::GC_PHASE_ENUM ||
        mutatorPhase == GCPhase::GC_PHASE_TRACE ||
        mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER;
    bool censusSafeMarking = mutatorPhase == GCPhase::GC_PHASE_PREFORWARD ||
        mutatorPhase == GCPhase::GC_PHASE_FORWARD ||
        (mutatorPhase == GCPhase::GC_PHASE_IDLE && !Heap::GetHeap().IsGcStarted());
    if (allocPtr == 0 || (!barrierClosedMarking && !censusSafeMarking)) {
        return allocPtr;
    }

    // Mark new allocated pinned object.
    BaseObject* object = from_alloc_addr(allocPtr);
    (reinterpret_cast<CopyCollector*>(&Heap::GetHeap().GetCollector()))->MarkObject(object);
    return allocPtr;
}

template void RegionManager::ForwardFromRegions<Generation::Young>(GCWorkers&);
template void RegionManager::ForwardFromRegions<Generation::Old>(GCWorkers&);
template void RegionManager::ForwardFromRegions<Generation::Young>();
template void RegionManager::ForwardFromRegions<Generation::Old>();
#if defined(MRT_TESTABLE_INTERNALS)
template class ForwardTask<Generation::Young>;
template class ForwardTask<Generation::Old>;
#endif
template void RegionManager::StartForwardFromRegions<Generation::Young>(GCWorkers&);
template void RegionManager::StartForwardFromRegions<Generation::Old>(GCWorkers&);
template void RegionManager::DrainForwardFromRegions<Generation::Young>();
template void RegionManager::DrainForwardFromRegions<Generation::Old>();
template void RegionManager::ForwardClaimedPage<Generation::Young>(RegionInfo*, ForwardingTable::Owner, bool);
template void RegionManager::ForwardClaimedPage<Generation::Old>(RegionInfo*, ForwardingTable::Owner, bool);
template void RegionManager::ForwardRegion<Generation::Young>(RegionInfo*);
template void RegionManager::ForwardRegion<Generation::Old>(RegionInfo*);
#if defined(MRT_GC_UNIT_TESTS)
// mc-r6: keep the unit-test-only mark-cycle entry points in the product
// carrier.  Tests must import these instantiations from libcangjie-runtime.so
// instead of instantiating a second copy in the test executable.
template void RegionInfo::ClearLiveInfo<Generation::Young>(MarkView<Generation::Young>);
template void RegionInfo::ClearLiveInfo<Generation::Old>(MarkView<Generation::Old>);
using GcUnitBumpSnapshotEpoch = void (RegionInfo::*)();
[[gnu::used]] static GcUnitBumpSnapshotEpoch const gcUnitBumpSnapshotEpoch =
    &RegionInfo::BumpSnapshotEpochFromInitRegion;
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
