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
void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    region->AddLiveCounts(1, obj->GetSize());
}

namespace {
// ZGenerationPagesIterator + ZPage::is_relocatable (zGeneration.cpp:209-213).
// Existing intrusive allocation lists are the Cangjie page-table adapter.
bool IsOldRelocationCandidate(const RegionInfo* region)
{
    return region->GetOwnerGeneration() == Generation::Old && region->IsRelocatable();
}
}

void RegionManager::AssembleSmallGarbageCandidates()
{
    // The young collection and old selection share the physical FROM list.
    // Establish the old selector's input here, before cleanup or forwarding.
    RegionInfo* region = fromRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            fromRegionList.DeleteRegion(region);
            ParkUnmovableFromRegion(region);
        }
        region = next;
    }
    auto select = [this](RegionList& list, bool recent) {
        RegionInfo* region = list.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextRegion();
            if (IsOldRelocationCandidate(region) && !region->IsNotRelocatableThisCycle()) {
                list.DeleteRegion(region);
                if (recent) RecentFullAccounting::Dequeue(1, region->GetUnitCount());
                fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            }
            region = next;
        }
    };
    select(rawPointerPinnedRegionList, false);
    select(recentFullRegionList, true);
    select(unmovableFromRegionList, false);
}

void RegionManager::AssembleLargeGarbageCandidates()
{
    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            oldLargeRegionList.DeleteRegion(region);
            recentLargeRegionList.PrependRegion(region, RegionInfo::RegionType::LARGE_REGION);
        }
        region = next;
    }
    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (IsOldRelocationCandidate(region)) {
            recentLargeRegionList.DeleteRegion(region);
            oldLargeRegionList.PrependRegion(region, RegionInfo::RegionType::LARGE_REGION);
        }
        region = next;
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


void RegionManager::AssemblePinnedGarbageCandidates(bool collectAll)
{
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!IsOldRelocationCandidate(region)) {
            oldPinnedRegionList.DeleteRegion(region);
            recentPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_PINNED_REGION);
        }
        region = next;
    }
    region = recentPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (IsOldRelocationCandidate(region)) {
            recentPinnedRegionList.DeleteRegion(region);
            oldPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::FULL_PINNED_REGION);
        }
        region = next;
    }
    region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (collectAll && region->GetRawPointerObjectCount() > 0) {
            oldPinnedRegionList.DeleteRegion(region);
            rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
        }
        region = next;
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
    // ZPage::is_allocating (zPage.inline.hpp:180-185) expires at the next
    // owning-generation sequence. LARGE pages stay on their existing lists;
    // their allocation watermark must nevertheless advance at young mark-start.
    auto snapshotYoungLarge = [](RegionList& list) {
        list.VisitAllRegions([](RegionInfo* page) {
            if (page->IsYoungRegion()) {
                page->ClearLiveInfo(page->GetMarkView<Generation::Young>());
            }
        });
    };
    snapshotYoungLarge(recentLargeRegionList);
    snapshotYoungLarge(oldLargeRegionList);
    snapshotYoungLarge(largeTraceRegions);
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

// Cost-model CSet (ZRelocationSetSelector.cpp:114-196) after mark, before flip.
// Semi-sort by per-page live fraction, then select the last profitable prefix.
size_t RegionManager::ExemptFromRegions()
{
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    std::vector<RegionInfo*> snapshot;
    fromRegionList.VisitAllRegions([&snapshot](RegionInfo* r) { snapshot.push_back(r); });
    std::vector<RelocRegionDesc> descs;
    std::vector<RegionInfo*> descRegions;
    descs.reserve(snapshot.size());
    descRegions.reserve(snapshot.size());
    for (RegionInfo* fromRegion : snapshot) {
        // ZGeneration::select_relocation_set (zGeneration.cpp:211-213).
        if (!fromRegion->IsRelocatable()) {
            if (ClaimFromRegion(fromRegionList, fromRegion, RegionInfo::RegionType::UNMOVABLE_FROM_REGION,
                                "allocating")) {
                ExemptFromRegion(fromRegion);
            }
            continue;
        }
        size_t liveBytes = fromRegion->GetLiveByteCount();
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        static constexpr bool kFreeEmptyAtCSetSelect = true;
        if (kFreeEmptyAtCSetSelect && liveBytes == 0 && rawPtrCnt == 0 &&
            !fromRegion->IsAllocating() && !fromRegion->IsYoungRegion()) {
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
                        static_cast<unsigned>(del->IsAllocating()),
                        static_cast<unsigned>(Heap::GetHeap().GetCollector().GetGCStats().reason),
                        static_cast<unsigned>(freeEmpty));
                }
            }
            if (!freeEmpty) {
                continue;
            }
            if (!ClaimFromRegion(fromRegionList, del, RegionInfo::RegionType::GARBAGE_REGION, "cset-empty")) {
                continue;
            }
            if (del->GetRawPointerObjectCount() > 0) {
                rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
                continue;
            }

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
            rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
            continue;
        }
        RelocRegionDesc d;
        d.liveBytes = liveBytes;
        d.capacity = fromRegion->GetRegionSize();
        d.kind = fromRegion->IsLargeRegion() ? RelocRegionKind::Large : RelocRegionKind::Small;
        d.id = static_cast<uint32_t>(descs.size());
        d.allocating = fromRegion->IsAllocating();
        descs.push_back(d);
        descRegions.push_back(fromRegion);
    }
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
        ExemptFromRegion(del);
        floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
    }

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t exemptedFromBytes = unmovableFromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "exempt from-space: %zu B - %zu B -> %zu B, %zu B floating garbage, %zu B to forward",
         oldFromBytes, exemptedFromBytes, newFromBytes, floatingGarbage, forwardBytes);
    return newFromBytes - forwardBytes;
}


} // namespace MapleRuntime
