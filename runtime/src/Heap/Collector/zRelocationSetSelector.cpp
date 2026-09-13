// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/zPageAllocator.hpp"

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
#include "Heap/Allocator/zForwardingTable.hpp"
#include "Heap/Collector/zRelocationSetSelector.hpp"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
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
    // ZGenerationOld::mark_start, zGeneration.cpp:1222.
    RetireSharedPages(kPageAgeRangeOld);
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


} // namespace MapleRuntime
