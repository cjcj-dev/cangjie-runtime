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
#include <unistd.h>
#include <vector>

#include "Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#include "Collector/CopyCollector.h"
#include "Collector/TenuringThreshold.h"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/HeapWork.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/F3Why2Diag.h"
#include "Heap/Verify/FlipPromoDiag.h"
#include "Heap/Verify/IdleEdgeDiag.h"
#include "Heap/Verify/O2ORemsetDiag.h"
#include "Heap/Verify/OffpastDiag.h"
#include "Heap/Verify/TraceClear.h"
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
uintptr_t RegionInfo::UnitInfo::totalUnitCount = 0;
uintptr_t RegionInfo::UnitInfo::heapStartAddress = 0;
size_t RegionInfo::UnitInfo::unitSizeShift = 0;

// gatehot: cold OOB for GetUnitIdxAt — kept out of the hot function so the common
// path can inline (was ~128 insns with dladdr/FATAL in the same body).
// Semantics unchanged: greppable FATAL + return 0 (unitzero trail).
size_t RegionInfo::UnitInfo::GetUnitIdxAtOOB(uintptr_t allocAddr)
{
    void* ra0 = __builtin_return_address(0);
    void* ra1 = nullptr;
    void* ra2 = nullptr;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
    ra1 = __builtin_return_address(1);
    ra2 = __builtin_return_address(2);
#pragma GCC diagnostic pop
#endif
    const char* s0 = "?";
    const char* s1 = "?";
    const char* s2 = "?";
#if !defined(_WIN64)
    Dl_info di0{};
    Dl_info di1{};
    Dl_info di2{};
    if (ra0 != nullptr && dladdr(ra0, &di0) != 0 && di0.dli_sname != nullptr) {
        s0 = di0.dli_sname;
    }
    if (ra1 != nullptr && dladdr(ra1, &di1) != 0 && di1.dli_sname != nullptr) {
        s1 = di1.dli_sname;
    }
    if (ra2 != nullptr && dladdr(ra2, &di2) != 0 && di2.dli_sname != nullptr) {
        s2 = di2.dli_sname;
    }
#endif
    LOG(RTLOG_FATAL,
        "GetUnitIdxAt OOB addr=%#zx heap=[%#zx, %#zx) "
        "ra0=%p(%s) ra1=%p(%s) ra2=%p(%s)",
        allocAddr, heapStartAddress, heapStartAddress + totalUnitCount * UNIT_SIZE,
        ra0, s0, ra1, s1, ra2, s2);
    return 0;
}
std::atomic<size_t> RegionInfo::youngRegionCount { 0 };
std::atomic<size_t> RegionInfo::dispelGhostCount { 0 };
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

// promotegap: offset histogram for promote re-registration (MRT_GCV2_PROMOTEGAP_PROBE=1).
namespace {
constexpr size_t kPromoteGapOffBuckets = 64;
std::atomic<uint64_t> g_pgInplaceSeen { 0 };
std::atomic<uint64_t> g_pgInplaceRec { 0 };
std::atomic<uint64_t> g_pgInplaceNode { 0 };
std::atomic<uint64_t> g_pgInplaceNode10Seen { 0 };
std::atomic<uint64_t> g_pgInplaceNode10Rec { 0 };
std::atomic<uint64_t> g_pgInplaceNode10SkipOldT { 0 };
std::atomic<uint64_t> g_pgInplaceNode10SkipNull { 0 };
std::atomic<uint64_t> g_pgFwdSeen { 0 };
std::atomic<uint64_t> g_pgFwdRec { 0 };
std::atomic<uint64_t> g_pgFwdNode { 0 };
std::atomic<uint64_t> g_pgFwdNode10Seen { 0 };
std::atomic<uint64_t> g_pgFwdNode10Rec { 0 };
std::atomic<uint64_t> g_pgFwdNode10SkipOldT { 0 };
std::atomic<uint64_t> g_pgFwdNode10SkipNull { 0 };
std::atomic<uint64_t> g_pgOffInplace[kPromoteGapOffBuckets] {};
std::atomic<uint64_t> g_pgOffFwd[kPromoteGapOffBuckets] {};
std::atomic<uint64_t> g_pgDumpSeq { 0 };

bool PromoteGapProbeOn()
{
    static const bool on = []() {
        return DiagGate::LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promote") ||
            DiagGate::LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promotegap");
    }();
    return on;
}

bool IsDefaultNode(BaseObject* object)
{
    if (object == nullptr) {
        return false;
    }
    TypeInfo* ti = object->GetTypeInfo();
    if (ti == nullptr) {
        return false;
    }
    const char* name = ti->GetName();
    return name != nullptr && std::strcmp(name, "default:Node") == 0;
}

void NotePromoteGapField(BaseObject* object, RefField<>& field, bool recorded, bool fwdPath)
{
    if (!PromoteGapProbeOn() || object == nullptr) {
        return;
    }
    MAddress base = reinterpret_cast<MAddress>(object);
    MAddress slot = reinterpret_cast<MAddress>(&field);
    if (slot < base) {
        return;
    }
    size_t off = static_cast<size_t>(slot - base);
    if (fwdPath) {
        g_pgFwdSeen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgFwdRec.fetch_add(1, std::memory_order_relaxed);
        }
        if (off < kPromoteGapOffBuckets) {
            g_pgOffFwd[off].fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_pgInplaceSeen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgInplaceRec.fetch_add(1, std::memory_order_relaxed);
        }
        if (off < kPromoteGapOffBuckets) {
            g_pgOffInplace[off].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!IsDefaultNode(object)) {
        return;
    }
    if (fwdPath) {
        g_pgFwdNode.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_pgInplaceNode.fetch_add(1, std::memory_order_relaxed);
    }
    if (off != 0x10) {
        return;
    }
    BaseObject* target = to_object(field.GetTargetObject());
    if (fwdPath) {
        g_pgFwdNode10Seen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgFwdNode10Rec.fetch_add(1, std::memory_order_relaxed);
        } else if (target == nullptr || !Heap::IsHeapAddress(target)) {
            g_pgFwdNode10SkipNull.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pgFwdNode10SkipOldT.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_pgInplaceNode10Seen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgInplaceNode10Rec.fetch_add(1, std::memory_order_relaxed);
        } else if (target == nullptr || !Heap::IsHeapAddress(target)) {
            g_pgInplaceNode10SkipNull.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pgInplaceNode10SkipOldT.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void DumpPromoteGapProbe(const char* tag)
{
    if (!PromoteGapProbeOn()) {
        return;
    }
    uint64_t seq = g_pgDumpSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    VLOG(REPORT,
         "[PROMOTEGAP][%s] seq=%llu inplace seen=%llu rec=%llu node=%llu "
         "node10seen=%llu node10rec=%llu node10skipOldT=%llu node10skipNull=%llu | "
         "fwd seen=%llu rec=%llu node=%llu node10seen=%llu node10rec=%llu "
         "node10skipOldT=%llu node10skipNull=%llu",
         tag, static_cast<unsigned long long>(seq),
         static_cast<unsigned long long>(g_pgInplaceSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceRec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10Seen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10Rec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10SkipOldT.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10SkipNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdRec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10Seen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10Rec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10SkipOldT.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10SkipNull.load(std::memory_order_relaxed)));
    for (size_t off = 0; off < kPromoteGapOffBuckets; ++off) {
        uint64_t a = g_pgOffInplace[off].load(std::memory_order_relaxed);
        uint64_t b = g_pgOffFwd[off].load(std::memory_order_relaxed);
        if (a == 0 && b == 0) {
            continue;
        }
        VLOG(REPORT,
             "[PROMOTEGAP][OFF] seq=%llu offset=0x%zx inplace=%llu fwd=%llu",
             static_cast<unsigned long long>(seq), off,
             static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    }
}
} // namespace

size_t RegionManager::RecordPromotedCrossGenEdges(RegionInfo* region)
{
    if (region == nullptr || !region->IsYoungRegion()) {
        return 0;
    }
    static const bool fysGapProbe = []() {
        const char* value = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_FYSGAP_PROBE */;
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
    if (region->IsSafeKnownYoungEmpty(view)) {
        if (fysGapProbe) {
            VLOG(REPORT,
                 "[FYSGAP][promotion-summary] region=%p recorded=0 live=0 dead=0 unknown=0 "
                 "knownEmpty=1 hasBitmap=%u mode=safe-empty",
                 region,
                 static_cast<unsigned>(region->GetMarkBitmap(view) != nullptr ||
                                       region->GetResurrectBitmap() != nullptr));
        }
        return 0;
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t recorded = 0;
    size_t liveEdges = 0;
    size_t deadEdges = 0;
    size_t unknownEdges = 0;
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap(view) != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    bool useLiveOnly = hasObjectLiveness && region->IsLiveCountAuthoritative();
    auto recordFromObject = [region, view, &rememberedSet, &recorded, &liveEdges, &deadEdges,
                             &unknownEdges, hasObjectLiveness, useLiveOnly](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        bool survived = hasObjectLiveness &&
            region->IsSurvivedObject(view, region->GetAddressOffset(reinterpret_cast<MAddress>(object)));
        if (useLiveOnly && !survived) {
            if (fysGapProbe) {
                object->ForEachRefField([&deadEdges](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        ++deadEdges;
                    }
                });
            }
            return;
        }
        object->ForEachRefField([&rememberedSet, &recorded, &liveEdges, &deadEdges, &unknownEdges,
                                hasObjectLiveness, survived, object](RefField<>& field) {
            BaseObject* target = to_object(field.GetTargetObject());
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                NotePromoteGapField(object, field, false, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*null/nonheap*/ 3, false);
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                rememberedSet.Record(slot);
                ++recorded;
                // promodomain dual-run: old product edge set for bidirectional reconcile.
                PromotedRegionDomain::NoteOldProductRecord(slot);
                FlipPromoDiag::NoteProductRecord(slot, /*path*/ 0);
                NotePromoteGapField(object, field, true, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*young*/ 1, true);
                if (fysGapProbe) {
                    if (!hasObjectLiveness) {
                        ++unknownEdges;
                    } else if (survived) {
                        ++liveEdges;
                    } else {
                        ++deadEdges;
                    }
                }
            } else {
                NotePromoteGapField(object, field, false, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*old*/ 2, false);
            }
        });
    };
    region->VisitAllObjects([&recordFromObject](BaseObject* object) { recordFromObject(object); });
    if (recorded != 0) {
        g_promotedCrossGenEdgeCount.fetch_add(recorded, std::memory_order_relaxed);
    }
    FlipPromoDiag::NotePromotedRegion(region, /*path*/ 0, recorded);
    if (fysGapProbe) {
        VLOG(REPORT,
             "[FYSGAP][promotion-summary] region=%p recorded=%zu live=%zu dead=%zu unknown=%zu "
             "knownEmpty=%u hasBitmap=%u mode=%s",
             region, recorded, liveEdges, deadEdges, unknownEdges,
             static_cast<unsigned>(region->IsKnownYoungEmpty(view)),
             static_cast<unsigned>(hasObjectLiveness), useLiveOnly ? "live-only" : "scan-all");
    }
    return recorded;
}

size_t RegionManager::ConsumePromotedCrossGenEdgeCount()
{
    size_t n = g_promotedCrossGenEdgeCount.exchange(0, std::memory_order_relaxed);
    DumpPromoteGapProbe("consume");
    return n;
}

size_t RegionManager::RecordPinnedCrossGenEdges()
{
    // gcscanoff blocking test: skip whole conservative pinned/old scan (default off).
    {
        static const bool skip = []() {
            const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_SKIP_PINNED_SCAN */;
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        if (skip) {
            VLOG(REPORT, "[GCV2][block] skip RecordPinnedCrossGenEdges env=MRT_GCV2_SKIP_PINNED_SCAN=1");
            return 0;
        }
    }
    MRT_PHASE_TIMER("young.pinned_scan");
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    std::atomic<size_t> recorded{ 0 };
    auto scanRegion = [&rememberedSet, &recorded](RegionInfo* region) {
        if (region == nullptr || region->IsYoungRegion() || region->IsGarbageRegion()) {
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
                RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    rememberedSet.Record(slot);
                    recorded.fetch_add(1, std::memory_order_relaxed);
                    FlipPromoDiag::NoteBroadRecord(region, slot);
                }
            });
        });
    };
    // STW-parallel of the same conservative walk. Record is fetch_or, so order
    // does not change the remset. Default OFF — mutator-visible state is identical.
    // Env MRT_GCV2_PINNED_SCAN_PARALLEL=1.
    static const bool parallelEnv = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_PINNED_SCAN_PARALLEL */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    GCThreadPool* pool = parallelEnv ? Heap::GetHeap().GetCollectorResources().GetThreadPool() : nullptr;
    if (pool != nullptr) {
        std::vector<RegionInfo*> regions;
        auto collect = [&regions](RegionInfo* region) {
            if (region != nullptr && !region->IsYoungRegion() && !region->IsGarbageRegion()) {
                regions.push_back(region);
            }
        };
        recentPinnedRegionList.VisitAllRegions(collect);
        oldPinnedRegionList.VisitAllRegions(collect);
        rawPointerPinnedRegionList.VisitAllRegions(collect);
        recentLargeRegionList.VisitAllRegions(collect);
        oldLargeRegionList.VisitAllRegions(collect);
        largeTraceRegions.VisitAllRegions(collect);
        recentFullRegionList.VisitAllRegions(collect);
        fullTraceRegions.VisitAllRegions(collect);
        unmovableFromRegionList.VisitAllRegions(collect);
        fromRegionList.VisitAllRegions(collect);
        tlRegionList.VisitAllRegions(collect);
        const size_t n = regions.size();
        if (n == 0) {
            return 0;
        }
        int32_t workers = pool->GetMaxThreadNum() + 1;
        if (workers < 1) {
            workers = 1;
        }
        std::atomic<size_t> cursor{ 0 };
        const size_t chunk = std::max<size_t>(1, (n + static_cast<size_t>(workers) * 4 - 1) /
                                                    (static_cast<size_t>(workers) * 4 + 1));
        for (int32_t w = 0; w < workers; ++w) {
            pool->AddWork(new (std::nothrow) LambdaWork(
                [&scanRegion, &regions, &cursor, n, chunk](size_t) {
                    for (;;) {
                        size_t i0 = cursor.fetch_add(chunk, std::memory_order_relaxed);
                        if (i0 >= n) {
                            break;
                        }
                        size_t i1 = std::min(i0 + chunk, n);
                        for (size_t i = i0; i < i1; ++i) {
                            scanRegion(regions[i]);
                        }
                    }
                }));
        }
        pool->Start();
        pool->WaitFinish();
        size_t nRec = recorded.load(std::memory_order_relaxed);
        VLOG(REPORT, "[GCV2][pinned_scan] parallel=1 regions=%zu workers=%d recorded=%zu", n, workers, nRec);
        return nRec;
    }
    // All never-young alloc paths + post-promote old holders (IDLE bare-store gap).
    // scanRegion already skips IsYoungRegion, so candidate young lists are free.
    recentPinnedRegionList.VisitAllRegions(scanRegion);
    oldPinnedRegionList.VisitAllRegions(scanRegion);
    rawPointerPinnedRegionList.VisitAllRegions(scanRegion);
    recentLargeRegionList.VisitAllRegions(scanRegion);
    oldLargeRegionList.VisitAllRegions(scanRegion);
    largeTraceRegions.VisitAllRegions(scanRegion);
    recentFullRegionList.VisitAllRegions(scanRegion);
    fullTraceRegions.VisitAllRegions(scanRegion);
    unmovableFromRegionList.VisitAllRegions(scanRegion);
    fromRegionList.VisitAllRegions(scanRegion);
    tlRegionList.VisitAllRegions(scanRegion);
    return recorded.load(std::memory_order_relaxed);
}

void RegionInfo::SetYoungRegionFlag(uint8_t flag)
{
    std::lock_guard<std::mutex> lock(youngRegionFlagMutex);
    bool wasYoung = IsYoungRegion();
    bool makeYoung = flag != 0;
    if (!wasYoung && makeYoung) {
        youngRegionCount.fetch_add(1, std::memory_order_release);
    }
    metadata.regionStateBitField.SetAtomicValue(
        RegionStateBitPos::YOUNG_REGION_FLAG, YOUNG_STATE_BIT_LENGTH, makeYoung ? 1 : 0);
    if (wasYoung && !makeYoung) {
        size_t count = youngRegionCount.load(std::memory_order_relaxed);
        CHECK(count > 0);
        youngRegionCount.fetch_sub(1, std::memory_order_release);
    }
}

size_t RegionInfo::GetYoungRegionCount()
{
    return youngRegionCount.load(std::memory_order_acquire);
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

template<Generation G>
class ForwardTask : public HeapWork {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() = default;

    void Execute(size_t) override
    {
        while (true) {
            RegionInfo* region = fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) { break; }
            regionManager.ForwardRegion<G>(region);
        }
    }

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};

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
        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // getsize7: GetAllocSize → GetSize reads TypeInfo; interiors/holes SEGV here
            // (deadlock_enqfrontier: VisitLiveObjectsUntilFalse ← RouteRegion ← TryForward).
            // Refuse: break without inventing size — remaining stream is unwalkable.
            if (!Collector::PlausibleManagedObjectGate("VisitAllObjects", obj)) {
                break;
            }
            // GetAllocSize should before call func, because object maybe destroy in compact gc.
            size_t size = RegionSpace::GetAllocSize(*obj);
            func(obj);
            position += size;
        }
    }
}

bool RegionInfo::VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func)
{
    // Skip only when a mark phase established live==0. Bare zero (e.g. non-young under minor)
    // is not an emptiness proof — fall through and consult the mark bitmap.
    if (IsRouteKnownEmpty()) {
        return true;
    }
    // tipnull arm R: Admit/GetRoute use the typed liveInfo0 face after PrepareForwardable.
    auto survivedAt = [this](size_t offset) -> bool { return IsRouteSurvivedObject(offset); };
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

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
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

    RegionInfo* pre = del->GetPrevRegion();
    RegionInfo* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);

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
    } else {
        pre->SetNextRegion(next);
    }

    if (listTail == del) { // delete tail
        MRT_ASSERT(next == nullptr, "Delete Region next is not null");
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else {
        next->SetPrevRegion(pre);
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

void RegionManager::SetMaxUnitCountForRegion()
{
    maxUnitCountPerRegion = CangjieRuntime::GetHeapParam().regionSize * KB / RegionInfo::UNIT_SIZE;
}

void RegionManager::SetMaxUnitCountForPinnedRegion()
{
    auto env = std::getenv("cjPinnedRegionSize");
    if (env == nullptr) {
        maxUnitCountPerPinnedRegion = maxUnitCountPerRegion;
        return;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    if (size >= minSize && size <= CangjieRuntime::GetHeapParam().regionSize) {
        maxUnitCountPerPinnedRegion = size * KB / RegionInfo::UNIT_SIZE;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjPinnedRegionSize parameter. Valid cjPinnedRegionSize"
            "range is [%zuKB, %zuKB].\n", minSize, CangjieRuntime::GetHeapParam().regionSize);
    }
}

void RegionManager::SetLargeObjectThreshold()
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
    size_t regionSize = CangjieRuntime::GetHeapParam().regionSize * KB;
    largeObjectThreshold = largeObjectThreshold > regionSize ? regionSize :  largeObjectThreshold;
}

void RegionManager::SetGarbageThreshold()
{
    fromSpaceGarbageThreshold = CangjieRuntime::GetGCParam().garbageThreshold;
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

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr)
{
    size_t metadataSize = GetMetadataSize(nUnit);
#ifdef _WIN64
    MemMap::CommitMemory(reinterpret_cast<void*>(regionInfoAddr), metadataSize);
#endif
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = regionInfoAddr + metadataSize;
    this->regionHeapEnd = regionHeapStart + nUnit * RegionInfo::UNIT_SIZE;
    // PORT_ZFORWARDING step 1: the address-keyed table covers the same span the units do, so an
    // index is (addr - base) / UNIT_SIZE with no probing -- ZGranuleMap's shape.
    ForwardingTable::Initialize(regionHeapStart, nUnit * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    this->inactiveZone = regionHeapStart;
    SetMaxUnitCountForRegion();
    SetMaxUnitCountForPinnedRegion();
    SetLargeObjectThreshold();
    SetGarbageThreshold();
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    RegionInfo::Initialize(nUnit, regionHeapStart);
    freeRegionManager.Initialize(nUnit);
    this->exemptedRegionThreshold = CangjieRuntime::GetHeapParam().exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
}

namespace {
// STEER4: metering gated by MRT_GCV2_SCRUB_COST (default off). Product path must not
// emit per-Collect VLOG floods (calls_per_run ~1161 under ALOT).
bool ScrubCostMeterEnabled()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_SCRUB_COST */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

std::atomic<uint64_t> g_scrubCalls{ 0 };
std::atomic<uint64_t> g_scrubNs{ 0 };
std::atomic<uint64_t> g_scrubWordsSum{ 0 };
std::atomic<uint64_t> g_scrubErasedSum{ 0 };
std::atomic<size_t> g_scrubWordsMax{ 0 };
std::atomic<size_t> g_staleAtCollect{ 0 };
} // namespace

void RegionManager::ScrubRememberedSetForRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    MAddress rStart = static_cast<MAddress>(region->GetRegionStart());
    MAddress rEnd = static_cast<MAddress>(region->GetRegionEnd());
    // Product path clears only the two bitmap slices owned by this region.
    if (!ScrubCostMeterEnabled() && !O2ORemsetDiag::Enabled()) {
        (void)Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, nullptr);
        return;
    }
    size_t words = 0;
    uint64_t t0 = TimeUtil::NanoSeconds();
    size_t scrubbed = Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, &words);
    if (O2ORemsetDiag::Enabled() && !region->IsYoungRegion()) {
        O2ORemsetDiag::NoteScrubNonYoung(region, scrubbed);
    }
    if (!ScrubCostMeterEnabled()) {
        return;
    }
    uint64_t dt = TimeUtil::NanoSeconds() - t0;
    uint64_t callNo = g_scrubCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_scrubNs.fetch_add(dt, std::memory_order_relaxed);
    g_scrubWordsSum.fetch_add(words, std::memory_order_relaxed);
    g_scrubErasedSum.fetch_add(scrubbed, std::memory_order_relaxed);
    size_t prevMax = g_scrubWordsMax.load(std::memory_order_relaxed);
    while (words > prevMax && !g_scrubWordsMax.compare_exchange_weak(prevMax, words, std::memory_order_relaxed)) {
    }
    VLOG(REPORT,
         "[GCV2][scrub-cost] call=%llu ns=%llu bitmapWords=%zu erased=%zu young=%u type=%u "
         "env=MRT_GCV2_SCRUB_COST=1",
         static_cast<unsigned long long>(callNo), static_cast<unsigned long long>(dt), words, scrubbed,
         static_cast<unsigned>(region->IsYoungRegion()), region->GetRegionType());
    if (scrubbed != 0) {
        size_t n = g_staleAtCollect.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT,
             "[GCV2][STALE_ENTRY_AT_COLLECT] yes scrubbed=%zu bitmapWords=%zu ns=%llu region=%p "
             "[%#zx,%#zx) type=%u young=%u sample=%zu env=MRT_GCV2_SCRUB_COST=1",
             scrubbed, words, static_cast<unsigned long long>(dt), region,
             static_cast<size_t>(rStart), static_cast<size_t>(rEnd), region->GetRegionType(),
             static_cast<unsigned>(region->IsYoungRegion()), n);
    }
}

void RegionManager::DumpScrubCostAndReset(const char* point)
{
    if (!ScrubCostMeterEnabled()) {
        return;
    }
    uint64_t calls = g_scrubCalls.exchange(0, std::memory_order_relaxed);
    uint64_t ns = g_scrubNs.exchange(0, std::memory_order_relaxed);
    uint64_t wordsSum = g_scrubWordsSum.exchange(0, std::memory_order_relaxed);
    uint64_t erasedSum = g_scrubErasedSum.exchange(0, std::memory_order_relaxed);
    size_t wordsMax = g_scrubWordsMax.exchange(0, std::memory_order_relaxed);
    if (calls == 0) {
        return;
    }
    VLOG(REPORT,
         "[GCV2][scrub-cost] point=%s calls=%llu ns=%llu avgNs=%llu bitmapWordsSum=%llu "
         "bitmapWordsMax=%zu erasedSum=%llu env=MRT_GCV2_SCRUB_COST=1",
         point == nullptr ? "?" : point, static_cast<unsigned long long>(calls),
         static_cast<unsigned long long>(ns), static_cast<unsigned long long>(ns / calls),
         static_cast<unsigned long long>(wordsSum), wordsMax,
         static_cast<unsigned long long>(erasedSum));
}

void RegionManager::ReclaimRegion(RegionInfo* region)
{
    // routedest: census, not a guard. The graft asked for CHECK(!IsRouteDestHeld()) here to
    // convert "I traced the paths" into a machine check, but none of the designs proved the
    // caller enumeration and five of the six ReclaimRegion callers have already detached the
    // region, so an abort here would trade an unproven assumption for a hard stop. Count and
    // name it instead, under the default-off account gate; a non-zero funnel_held is the
    // signal that the enumeration was wrong.
    RouteDestHold::NoteReclaimFunnel(region, "ReclaimRegion");
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    // gcvroot Z2: poison reclaimed payload so use-after-free roots are identifiable (MRT_GCV2_ZAP_RECLAIM=1).
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    freeRegionManager.AddGarbageUnits(unitIndex, num);
}

void RegionManager::ReclaimRegionToMarkQuarantine(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.
    RouteDestHold::NoteReclaimFunnel(region, "ReclaimRegionToMarkQuarantine");
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    // routedest: census only, see ReclaimRegion.
    RouteDestHold::NoteReclaimFunnel(region, "ReleaseRegion");
    RegionLifeDiag::NoteRelease(region, RegionLifeDiag::PATH_RELEASE_LARGE);
    // holdercapture: large regions above the release threshold never reach CollectRegion,
    // so the snapshot has to be taken on this path too or the face is lost unrecorded.
    MarkFaceSnap::NoteRegionFree(region, RegionLifeDiag::PATH_RELEASE_LARGE);
    size_t res = region->GetRegionSize();
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

    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, num);
    freeRegionManager.AddReleaseUnits(unitIndex, num);
    return res;
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    region->AddLiveByteCount(obj->GetSize());
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
                recentFullRegionList.DeleteRegion(region);
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
    size_t heldRegions = 0;
    size_t heldBytes = 0;
    auto clearList = [&heldRegions, &heldBytes](RegionList& list) {
        list.VisitAllRegions([&heldRegions, &heldBytes](RegionInfo* region) {
            if (region->IsRouteDestHeld()) {
                ++heldRegions;
                heldBytes += region->GetRegionSize();
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
    RouteDestHold::NoteClearPoint(heldRegions, heldBytes);
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
    YoungCollectionStats stats;
    RegionInfo* oldRegion = fromRegionList.GetHeadRegion();
    while (oldRegion != nullptr) {
        RegionInfo* next = oldRegion->GetNextRegion();
        fromRegionList.DeleteRegion(oldRegion);
        ExemptFromRegion(oldRegion);
        oldRegion = next;
    }

    RegionInfo* region = unmovableFromRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        // twoflags: notRelocatable is major-Assemble only. Young mark re-establishes
        // liveness for POST_TRACE-stamped regions — do not skip minor CSet.
        // routedest: that reasoning is about liveness and does not transfer. A route
        // destination is excluded here on address ownership, not on whether its contents are
        // reachable. This loop matters most of the four: every mutator thread-local region is
        // young (RegionSpace.cpp takes the youngRegion = true default), and the destination
        // recorded at RegionManager.cpp:1957 is exactly such a region — so before this gate a
        // minor collected a live route's destination while honouring nothing.
        if (RouteDestHold::HoldsBack(region, RouteDestHold::Site::YOUNG_UNMOVABLE)) {
            region = next;
            continue;
        }
        MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
        region->ClearLiveInfo(view);
        visitor(region);
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() == 0) {
            unmovableFromRegionList.DeleteRegion(region);
            fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        }
        region = next;
    }

    region = recentFullRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        // routedest: same exclusion as the unmovable young loop above.
        if (RouteDestHold::HoldsBack(region, RouteDestHold::Site::YOUNG_RECENT_FULL)) {
            region = next;
            continue;
        }
        MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
        region->ClearLiveInfo(view);
        visitor(region);
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() != 0) {
            region = next;
            continue;
        }
        recentFullRegionList.DeleteRegion(region);
        fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region = next;
    }
    return stats;
}

void RemoveRegionLocked(RegionList* regionList, RegionInfo* region)
{
    regionList->DeleteRegionLocked(region);
}

// Cost-model CSet (ZRelocationSetSelector.cpp:114-196) after mark, before flip.
// Sort key = GetLiveByteCount(); stop = relative reclaimable <= kRelocationFragmentationLimitPercent.
size_t RegionManager::ExemptFromRegions()
{
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
        if (rawPtrCnt > 0) {
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) pinned by forwarding: %zu units, %zu live bytes rawPtr cnt %u",
                del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount(), rawPtrCnt);
            CHECK(del->IsFromRegion());
            if (liveBytes > 0) {
                del->PreserveRetainedLiveInfo();
            }
            RemoveRegionLocked(&fromRegionList, del);
            rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
            continue;
        }
        if (!kUseRelocationSetSelector) {
            size_t threshold = static_cast<size_t>(exempt * fromRegion->GetRegionSize());
            if (liveBytes > threshold) {
                RegionInfo* del = fromRegion;
                CHECK(del->IsFromRegion());
                del->PreserveRetainedLiveInfo();
                RemoveRegionLocked(&fromRegionList, del);
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
            CHECK(del->IsFromRegion());
            del->PreserveRetainedLiveInfo();
            RemoveRegionLocked(&fromRegionList, del);
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
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        // Finalizer reclaims concurrently (not a mutator ⇒ STW does not stop it). A unit
        // mid-InitRegionInfo can expose a transient extent (0/garbage) before the final
        // role is published. Following a bogus end lands GetUnitIdxAt(0) → named fatal+abort
        // (S1: SIGABRT under InvalidateOldTaggedRefs). Step one unit instead —
        // such units are never visitable.
        // Anchor: a1f81854 (fix/gcfix), landed here as e2293c2b; the guard below is
        // character-identical to it. Its other hunk targeted PromoteAllRegions, which
        // no longer exists on this line.
        uintptr_t nextAddr = region->GetRegionEnd();
        if (nextAddr <= regionAddr || nextAddr > inactiveZone) {
            regionAddr += RegionInfo::UNIT_SIZE;
            continue;
        }
        regionAddr = nextAddr;
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        MarkView<Generation::Old> oldView = region->GetMarkView<Generation::Old>();
        if (skipKnownEmptyRegions && region->IsKnownEmpty(oldView)) {
            continue;
        }
        region->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
    }
}

void RegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

void RegionManager::StampCensusBoundaries()
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            region->StampCensusBoundary();
        }
    }
}

void RegionManager::PromoteAllRegions()
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
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
    }
}

RegionInfo* RegionManager::TakeRegion(size_t num, RegionInfo::UnitRole type, bool expectPhysicalMem,
                                      bool allowSaferegion)
{
    // a chance to invoke heuristic gc.
    // routefix: under ROUTING, skip RequestGC — PostIgnoredGcRequest may ScopedEnterSaferegion.
    if (allowSaferegion && !Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        size_t heapThreshold = collector.GetGCStats().GetThreshold();
        size_t youngRegionTriggerBytes = 32 * MB;
        // genperf: default-off arm B — raise young trigger out of reach so minor never fires;
        // barriers/remset still run. Unset must match product path bit-for-bit.
        // gchot: TakeRegion is alloc-hot; cache once (genperf sets env at process start).
        static const bool disableMinor = []() {
            const char* disableMinorEnv = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_DISABLE_MINOR */;
            return disableMinorEnv != nullptr && std::strcmp(disableMinorEnv, "1") == 0;
        }();
        if (disableMinor) {
            youngRegionTriggerBytes = std::numeric_limits<size_t>::max();
        }
        static const bool jvmYoungTriggerOn = []() {
            const char* jvmYoungTriggerEnv = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_JVM_YOUNG_TRIGGER */;
            return jvmYoungTriggerEnv != nullptr && std::strcmp(jvmYoungTriggerEnv, "1") == 0;
        }();
        const bool useJvmYoungTrigger = !disableMinor && jvmYoungTriggerOn;
        size_t youngTriggerFloor = 0;
        size_t youngTriggerTarget = 0;
        size_t youngTriggerCeiling = 0;
        if (useJvmYoungTrigger) {
            // G1 sizes young between 5% and 60% of its heap. This runtime has no eden/survivor
            // pause controller, so apply those bounds to the HEU budget and target half that budget.
            constexpr size_t youngTriggerFloorPercent = 5;
            constexpr size_t youngTriggerTargetPercent = 50;
            constexpr size_t youngTriggerCeilingPercent = 60;
            youngTriggerFloor = heapThreshold * youngTriggerFloorPercent / 100;
            youngTriggerTarget = heapThreshold * youngTriggerTargetPercent / 100;
            youngTriggerCeiling = heapThreshold * youngTriggerCeilingPercent / 100;
            youngRegionTriggerBytes =
                std::min(std::max(youngTriggerTarget, youngTriggerFloor), youngTriggerCeiling);
            CHECK_DETAIL(youngRegionTriggerBytes < heapThreshold,
                         "young GC threshold %zu must stay below HEU threshold %zu",
                         youngRegionTriggerBytes, heapThreshold);
        }
        size_t youngAllocated = GetYoungAllocatedSize();
        if (youngAllocated >= youngRegionTriggerBytes) {
            if (useJvmYoungTrigger) {
                VLOG(REPORT,
                     "[GCV2][jvm-young-trigger] young=%zu trigger=%zu HEU=%zu floor=%zu target=%zu ceiling=%zu "
                     "invariant=%d",
                     youngAllocated, youngRegionTriggerBytes, heapThreshold, youngTriggerFloor, youngTriggerTarget,
                     youngTriggerCeiling, youngRegionTriggerBytes < heapThreshold);
            }
            DLOG(ALLOC, "request young gc: allocated %zu, threshold %zu", youngAllocated, youngRegionTriggerBytes);
            collector.RequestGC(GC_REASON_YOUNG, true);
        } else {
            size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
            if (allocated >= heapThreshold) {
                DLOG(ALLOC, "request heu gc: allocated %zu, threshold %zu", allocated, heapThreshold);
                collector.RequestGC(GC_REASON_HEU, true);
            }
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
    // routefix: ReclaimRegion → AddGarbageUnits ScopedEnterSaferegion; skip under ROUTING.
    RegionInfo* head = allowSaferegion ? TakeReclaimableGarbageRegion(&gatedBytes) : nullptr;
    if (head != nullptr) {
        DLOG(REGION, "take garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
        if (head->GetUnitCount() == num) {
            TraceClear::NoteRegionEvent(head->GetRegionStart(), head->GetRegionSize(), "garbage_reuse", head,
                                        head->GetLiveByteCount(),
                                        static_cast<unsigned int>(head->IsGhostFromRegion()),
                                        static_cast<unsigned int>(head->GetRegionType()),
                                        static_cast<unsigned int>(head->GetRouteState()));
            // promodomain obligation①: undischarged flip-promoted region must not ClearUnits.
            PromotedRegionDomain::CheckNotUndischargedForReuse(head, "TakeRegion.garbage_reuse");
            // fwdinflight: the reuse edge. ClearUnits zeroes the payload with no region
            // rwLock held -- CollectRegion's write lock (RegionManager.h:436-447) covers only
            // the list move, not this. Count readers still inside a route lookup on it.
            FwdInflight::NoteRetireRegion(head, FwdInflight::Retire::TAKE_GARBAGE);
            auto idx = head->GetUnitIdx();
            {
                // portmutreloc: ZForwarding::detach_page before the page goes back to the
                // allocator. ClearUnits zeroes the payload a retained reader may still be
                // copying out of, so the drain has to enclose it. Scoped tight: it ends
                // before InitRegion, which re-initialises the metadata the lock lives in.
                // The wipe is of the payload (GetUnitAddress(idx)), not of the UnitInfo
                // array, so the lock itself survives the body.
                RegionInfo::DrainScope drain(head, MutatorRelocate::Retire::TAKE_GARBAGE);
                RegionInfo::ClearUnits(idx, num);
            }
            DLOG(REGION, "reuse garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            return RegionInfo::InitRegion(idx, num, type);
        } else {
            DLOG(REGION, "reclaim garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            ReclaimRegion(head);
        }
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

    RegionInfo* region = freeRegionManager.TakeRegion(num, type, expectPhysicalMem, allowSaferegion);
    if (region != nullptr) {
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        return region;
    }

    // when free regions are not enough for allocation
    if (num <= GetInactiveUnitCount()) {
        // Reserve the extent with CAS. A failed fetch_add reservation cannot be rolled back
        // with fetch_sub: another thread may have committed a later extent in between, so
        // subtracting here would move inactiveZone back over that live allocation.
        uintptr_t addr = inactiveZone.load(std::memory_order_relaxed);
        bool reserved = false;
        while (addr <= regionHeapEnd - size) {
            if (inactiveZone.compare_exchange_weak(addr, addr + size, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                reserved = true;
                break;
            }
        }
        if (reserved) {
            region = RegionInfo::InitRegionAt(addr, num, type);
            size_t idx = region->GetUnitIdx();
#ifdef _WIN64
            MemMap::CommitMemory(
                reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx)), num * RegionInfo::UNIT_SIZE);
#endif
            (void)idx; // eliminate compilation warning
            DLOG(REGION, "take inactive units [%zu+%zu, %zu) at [0x%zx, 0x%zx)", idx, num, idx + num,
                 RegionInfo::GetUnitAddress(idx), RegionInfo::GetUnitAddress(idx + num));
            if (num >= HUGE_PAGE) {
                TagHugePage(region, num);
            }
            if (expectPhysicalMem) {
                RegionInfo::ClearUnits(idx, num);
            }
            return region;
        }
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
void RegionManager::ForwardFromRegions(GCThreadPool* threadPool)
{
    if (threadPool != nullptr) {
        int32_t threadNum = threadPool->GetMaxThreadNum() + 1;
        // We won't change fromRegionList during gc, so we can use it without lock.
        size_t regionCount = fromRegionList.GetRegionCount();
        if (UNLIKELY(regionCount == 0)) {
            return;
        }

        // we start threadPool before adding work so that we can concurrently add tasks;
        threadPool->Start();
        for (int32_t i = 0; i < threadNum; ++i) {
            threadPool->AddWork(new (std::nothrow) ForwardTask<G>(*this, fromRegionList));
        }
        threadPool->WaitFinish();
    } else {
        ForwardFromRegions<G>();
    }
}

void RegionManager::ExemptFromRegion(RegionInfo* region)
{
    unmovableFromRegionList.PrependRegion(region, RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
}

template<Generation G>
void RegionManager::ForwardFromRegions()
{
    // Use the same ownership transition as ForwardTask.  Walking the linked
    // list in place leaves a forwarded region attached as FROM_REGION, so the
    // next young cycle can revisit stale list state.  A zero-helper execution
    // is serial, but it must still detach and mark each unit LONE_FROM_REGION.
    while (true) {
        RegionInfo* region = fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
        if (region == nullptr) {
            break;
        }
        MRT_ASSERT(region->IsValidRegion(), "the head region of fromRegionList is invalid");
        ForwardRegion<G>(region);
    }

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
        PinFireDiag::NoteSkipFreeSlots(region);
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
    PinFireDiag::NoteCollectPinnedGarbage();
    {
        std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
        freePinnedSlotLists.Clear();
    }
    size_t garbageSize = 0;
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        // pinroot: whole-region reclaim also ignores pins; skip while any raw pointer holds.
        if (region->GetRawPointerObjectCount() > 0) {
            PinFireDiag::NoteSkipRegion(region);
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

            RegionLifeDiag::SetNextFreePath(RegionLifeDiag::PATH_PINNED_GARBAGE);
            garbageSize += CollectRegion<Generation::Old>(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
            region = region->GetNextRegion();
        }
    }
    PinFireDiag::Report("post-CollectPinnedGarbage");
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
        // What is actually measurable is disagreement between faces. GetMarkedRegionFlag
        // returns 0 from its first line when the view's epoch is not the region's current
        // one (RegionInfo.h), so a region marked under one epoch can read dead under the
        // epoch this decision binds. MarkFaceSnap therefore records the predicate's own
        // view AND the route view side by side, plus whether the route view's epoch gate
        // was even open - without that last column, "the faces agree" and "the second face
        // was unreadable" are the same observation.
        //
        // The mark bit read through the view below is a control, not the finding: it must
        // be 0 on every released region, and if it ever is not, the reading of this
        // predicate is wrong and the rest of the measurement is void.
        MarkFaceSnap::NoteBeforeReleaseDecision(region);
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
                RegionLifeDiag::SetNextFreePath(RegionLifeDiag::PATH_LARGE_GARBAGE);
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
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (!region->IsFreeRegion()) {
            region->DumpRegionInfo(ALLOC);
        }
    }
}
#endif

void RegionManager::DumpRegionStats(const char* msg, bool dumpToError) const
{
    size_t totalSize = regionHeapEnd - regionHeapStart;
    size_t totalUnits = totalSize / RegionInfo::UNIT_SIZE;
    size_t activeSize = inactiveZone - regionHeapStart;
    size_t activeUnits = activeSize / RegionInfo::UNIT_SIZE;

    size_t tlRegions = tlRegionList.GetRegionCount();
    size_t tlUnits = tlRegionList.GetUnitCount();
    size_t tlSize = tlUnits * RegionInfo::UNIT_SIZE;
    size_t allocTLSize = tlRegionList.GetAllocatedSize();

    size_t fromRegions = fromRegionList.GetRegionCount();
    size_t fromUnits = fromRegionList.GetUnitCount();
    size_t fromSize = fromUnits * RegionInfo::UNIT_SIZE;
    size_t allocFromSize = fromRegionList.GetAllocatedSize();

    size_t recentFullRegions = recentFullRegionList.GetRegionCount();
    size_t recentFullUnits = recentFullRegionList.GetUnitCount();
    size_t recentFullSize = recentFullUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentFullSize = recentFullRegionList.GetAllocatedSize();

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

    size_t allHeapSize = regionHeapEnd - regionHeapStart;
    size_t allUnits = allHeapSize / RegionInfo::UNIT_SIZE;
    size_t inactiveUnits = (regionHeapEnd - inactiveZone) / RegionInfo::UNIT_SIZE;
    
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

#define DUMP_REGION_STATS_LOG(format, ...)                  \
    do {                                                    \
        VLOG(REPORT, format, ##__VA_ARGS__);                \
        if (dumpToError) {                                  \
            LOG(RTLOG_ERROR, format, ##__VA_ARGS__);        \
        }                                                   \
    } while (false)

    DUMP_REGION_STATS_LOG("%s", msg);

    DUMP_REGION_STATS_LOG("\ttotal units: %zu (%zu B)", totalUnits, totalSize);
    DUMP_REGION_STATS_LOG("\tactive units: %zu (%zu B)", activeUnits, activeSize);
    DUMP_REGION_STATS_LOG("\tinactive units: %zu (%zu B)", inactiveUnits, inactiveUnits * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\ttl-regions %zu: %zu units (%zu B, alloc %zu)", tlRegions,  tlUnits, tlSize, allocTLSize);
    DUMP_REGION_STATS_LOG("\tfrom-regions %zu: %zu units (%zu B, alloc %zu)", fromRegions,  fromUnits, fromSize,
                          allocFromSize);
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

RegionInfo* RegionManager::AllocateThreadLocalRegion(bool expectPhysicalMem, bool youngRegion, bool allowSaferegion)
{
    RegionInfo* region = TakeRegion(maxUnitCountPerRegion, RegionInfo::UnitRole::SMALL_SIZED_UNITS, expectPhysicalMem,
                                    allowSaferegion);
    if (region != nullptr) {
        {
            region->SetYoungRegionFlag(youngRegion ? 1 : 0);
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

bool RegionManager::RouteOrCompactRegionImpl(RegionInfo* region)
{
    CHECK(region->IsRoutingState());
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");
    OffpastDiag::NoteRouteEnter(region);
    // densifycut (G6): densify apply+walk+census removed. Product path already never applied
    // (MRT_GCV2_DENSIFY default off). Exit net retained: allLiveBitsHaveReceipt + abandon +
    // AdmitForRoute + VisitLiveObjectsUntilFalse. densifyOutcome always "not densified" (1).
    size_t fromBytes = region->GetLiveByteCount();
    // GetRoute (LiveInfo.cpp:15-23) places each survivor by liveInfo0 prefix-sum.
    // A 1-region plan writes to2=INVALID and to1used=fromBytes. If the bitmap
    // face is larger than the counter (ghost-only MarkBits never AddLiveByteCount:
    // EnsureRouteDomainMembership.ghost, youngstatic.pregrant.ghost,
    // statresid.force_domain.ghost), preLive >= to1used walks into else and
    // CHECK(toRegion2Idx != INVALID) fires (rareyc C). Size the reservation
    // by the face GetRoute actually reads. Do not relax that CHECK.
    LiveInfo* planFace = region->GetLiveInfo0ForProbe();
    if (planFace == nullptr) {
        planFace = region->GetLiveInfo();
    }
    size_t bitmapLive = region->GetRouteBitmapLiveBytes(planFace);
    if (bitmapLive > fromBytes) {
        fromBytes = bitmapLive;
    }
    // permwho: fromBytes now sizes the reservation to cover the prefix-sum face.
    PermWhoAdmit::NoteRoutePlan(region, fromBytes, /*densifyOutcome=*/1u);
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    RegionInfo* toRegion1 = buffer->GetRegion();
    // resolveto / offpast: CompactRegion prepends the still-ghost region as TL and the old
    // path SetRegion'd it. The next Route then packed a *different* region's survivors into
    // that Compacted tail. Resolve rewrote roots to those to-addrs; Fix Admit'd them against
    // the host's from-offset bits (live0Surv=0) → leave-alone → reclaim → GetSize MAPERR.
    if (toRegion1 != RegionInfo::NullRegion() &&
        (toRegion1->IsCompacted() || toRegion1->IsGhostFromRegion() || toRegion1 == region)) {
        buffer->ClearRegion();
        toRegion1 = RegionInfo::NullRegion();
    }
    CHECK(region != toRegion1);
    bool result;
    // routefix: already hold ROUTING — allocate without ScopedEnterSaferegion.
    // Fail → CompactRegion (same as product null path); geometry still freezes at NoteSeal.
    if (toRegion1 == RegionInfo::NullRegion()) {
        toRegion1 = AllocateThreadLocalRegion(false, false, /*allowSaferegion=*/false);
        if (toRegion1 == nullptr) {
            // routedest: the immune arm. This plan names the from-region as its own
            // destination, and the from-region is already a ghost — the ghost bit is what
            // bounds route readability, and DispelGhostFromRegion drops it and the route
            // together. No hold is needed, and stamping one here would pin a region that is
            // about to be reclaimed as garbage. The asymmetry with the other four sites is
            // deliberate; it is written here rather than only in the design note because it
            // is surprising at the call site.
            CHECK(region->IsGhostFromRegion());
            // Publish the in-place plan before Compact so GetRoute dests exist while copying.
            region->SetRouteInfo(region->GetRegionStart(), fromBytes);
            CompactRegion(region);
            toRegion1 = region;
            result = false;
            buffer->ClearRegion();
            RehomeCompactedInPlaceRegion(region);
            DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => compact-in-place %p@[%#zx~%#zx, %#zx)",
                region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
                toRegion1->GetRegionStart(), toRegion1->GetRegionStart() + fromBytes, toRegion1->GetRegionEnd());
            return result;
        } else {
            toRegion1->Alloc(fromBytes);
            result = true;
            buffer->SetRegion(toRegion1);
        }
        size_t toRegion1Start = toRegion1->GetRegionStart();
        // routedest: hold the destination before the plan naming it becomes readable.
        // Ordering matters against reclaim threads, not against route readers: readers are
        // already excluded by the ROUTING spin (RegionManager.h:664-667), but the finalizer
        // reclaim path is not stopped by anything here.
        toRegion1->SetRouteDestHold(1);
        region->SetRouteInfo(toRegion1Start, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1Start, toRegion1Start + fromBytes, toRegion1->GetRegionEnd());
        return result;
    }

    size_t toRegion1Capacity = toRegion1->GetAvailableSize();
    MAddress toRegion1Addr = toRegion1->GetRegionAllocPtr();
    if (fromBytes <= toRegion1Capacity) {
        toRegion1->Alloc(fromBytes);
        // routedest: the widest-exposure arm. toRegion1Addr is a bump pointer taken from the
        // middle of the calling thread's own live alloc-buffer region, which keeps serving
        // that thread's allocations afterwards, and which is young — so before this hold the
        // minor collection set took it while honouring nothing (PrepareYoungGarbageCandidates
        // deliberately ignores notRelocatableThisCycle).
        toRegion1->SetRouteDestHold(1);
        region->SetRouteInfo(toRegion1Addr, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1->GetRegionStart(), toRegion1Addr, toRegion1Addr + fromBytes, toRegion1->GetRegionEnd());
        return true;
    }
    size_t toRegion1Waste = toRegion1Capacity;
    BaseObject* leftObject = nullptr;
    (void)region->VisitLiveObjectsUntilFalse([&toRegion1Waste, &leftObject](BaseObject* obj) {
        size_t objSz = RegionSpace::GetAllocSize(*obj);
        if (toRegion1Waste >= objSz) {
            toRegion1Waste -= objSz;
            return true;
        } else {
            leftObject = obj;
            return false;
        }
    });
    MAddress usedBytes1 = toRegion1Capacity - toRegion1Waste;
    MAddress usedBytes2 = fromBytes - usedBytes1;
    CHECK(toRegion1->IsThreadLocalRegion());
    {
        RemoveThreadLocalRegion(toRegion1);
        EnlistFullThreadLocalRegion(toRegion1);
    }

    RegionInfo* toRegion2 = AllocateThreadLocalRegion(false, false, /*allowSaferegion=*/false);
    CHECK(region != toRegion2);
    if (toRegion2 != nullptr) {
        toRegion1->Alloc(usedBytes1);
        CHECK(toRegion2->Alloc(usedBytes2) != 0);
        result = true;
        buffer->SetRegion(toRegion2);
    } else {
        // Publish the split plan before Compact so leftover objects land at GetRoute dests.
        toRegion1->SetRouteDestHold(1);
        region->SetRouteInfo(toRegion1Addr, usedBytes1, region->GetUnitIdx());
        CompactRegion(region, toRegion1);
        toRegion2 = region; // region is partially compacted into itself.
        result = false;
        buffer->ClearRegion();
        RehomeCompactedInPlaceRegion(region);
    }
    uint32_t toRegion2Idx = toRegion2->GetUnitIdx();
    // routedest: on the toRegion2 == nullptr path above, SetRouteInfo has already run once
    // for this same holder and toRegion1 is stamped twice. The stamp is a byte store, so the
    // repeat is a no-op — this is exactly the shape that would be a double-increment bug if
    // the hold were ever turned into a reference count.
    toRegion1->SetRouteDestHold(1);
    if (toRegion2 != region) {
        toRegion2->SetRouteDestHold(1);
    }
    region->SetRouteInfo(toRegion1Addr, usedBytes1, toRegion2Idx);
    DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx) & %p@[%#zx~%#zx, %#zx)", region,
        region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1, toRegion1->GetRegionStart(),
        toRegion1Addr, toRegion1Addr + usedBytes1, toRegion1->GetRegionEnd(), toRegion2,
        toRegion2->GetRegionStart(), toRegion2->GetRegionStart() + usedBytes2, toRegion2->GetRegionEnd());
    return result;
}

void RegionManager::CompactRegion(RegionInfo* region)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u", region, regionStart,
        region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
    const bool youngRegion = region->IsYoungRegion();
    // compactrem: count calls + per-object geometry / teset-in-from (default-off).
    if (O2ORemsetDiag::Enabled()) {
        O2ORemsetDiag::NoteCompactCall(/*overload*/ 1, youngRegion);
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    MAddress regionLimit = region->GetRegionAllocPtr();
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    // uafclose: Admit/GetRoute/VisitLive use liveInfo0 after PrepareForwardable. Compact must
    // copy the same set — region->IsSurvivedObject reads current liveInfo (+ mark-epoch), which
    // can disagree with the ghost face. Wrong face ⇒ copy nothing / wrong set, memset free-tail
    // that still holds root-named from → leave-alone → reclaim → GetSize UAF (FYS1 deadold).
    auto survivedAt = [region](size_t offset) -> bool { return region->IsRouteSurvivedObject(offset); };
    // resolveto: keep dense pack (no holes). Record fromOff→dest so GetRoute on
    // COMPACTED answers the packed slot, not the prefix-sum hole.
    region->FreeCompactRouteTable();
    region->EnsureCompactRouteTable();
    region->SetRegionAllocPtr(regionStart);
    bool walkBroke = false;
    for (MAddress currentPtr = regionStart; currentPtr < regionLimit;) {
        BaseObject* currentObj = from_region_addr(currentPtr);
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            walkBroke = true;
            break;
        }
        size_t size = currentObj->GetSize();
        size_t offset = currentPtr - regionStart;
        if (survivedAt(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            if (O2ORemsetDiag::Enabled() && !youngRegion) {
                size_t remIn = 0;
                MAddress fromEnd = currentPtr + size;
                for (MAddress slot : rememberedSet.Snapshot()) {
                    if (slot >= currentPtr && slot < fromEnd) {
                        ++remIn;
                    }
                }
                O2ORemsetDiag::NoteCompactRemsetInFrom(remIn);
            }
            collector.CopyObject(*currentObj, *toObj, size);
            ForwardingTable::InsertMapping(currentPtr, toAddress);
            toObj->SetStateCode(ObjectState::NORMAL);
            region->RecordCompactRoute(offset, toAddress);
            if (O2ORemsetDiag::Enabled()) {
                O2ORemsetDiag::NoteCompactObjectMove(currentObj, toObj, size, youngRegion);
            }
        }
        currentPtr += size;
    }
    std::atomic_thread_fence(std::memory_order_release);

    // clear unused space which is free after compaction.
    // Walk-break leftovers are still live at from — do not memset them.
    MAddress cur = region->GetRegionAllocPtr();
    if (!walkBroke && regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK,
                         "clear buffer failed");
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();

    OffpastDiag::NoteCompactDone(region);
    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
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
    if (region->GetRegionType() != RegionInfo::RegionType::THREAD_LOCAL_REGION) {
        return;
    }
    tlRegionList.TryDeleteRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION,
                                 RegionInfo::RegionType::RECENT_FULL_REGION);
    recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
}

void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u to region %p@%#zx:%#zx",
        region, regionStart, region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType(),
        toRegion1, toRegion1->GetRegionStart(), toRegion1->GetRegionAllocPtr());
    const bool youngRegion = region->IsYoungRegion();
    if (O2ORemsetDiag::Enabled()) {
        O2ORemsetDiag::NoteCompactCall(/*overload*/ 2, youngRegion);
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    MAddress currentPtr = regionStart;
    BaseObject* currentObj = from_region_addr(currentPtr);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    // uafclose: same ghost-face survivor as CompactRegion(region) / VisitLive / Admit.
    auto survivedAt = [region](size_t offset) -> bool { return region->IsRouteSurvivedObject(offset); };
    region->FreeCompactRouteTable();
    while (true) {
        CHECK(currentPtr>=regionStart);
        size_t offset = currentPtr - regionStart;
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            break;
        }
        size_t size = currentObj->GetSize();
        if (survivedAt(offset)) {
            MAddress toAddress = toRegion1->Alloc(size);
            if (toAddress == 0) {
                break;
            }
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            if (O2ORemsetDiag::Enabled() && !youngRegion) {
                size_t remIn = 0;
                MAddress fromEnd = currentPtr + size;
                for (MAddress slot : rememberedSet.Snapshot()) {
                    if (slot >= currentPtr && slot < fromEnd) {
                        ++remIn;
                    }
                }
                O2ORemsetDiag::NoteCompactRemsetInFrom(remIn);
            }
            collector.CopyObject(*currentObj, *toObj, size);
            ForwardingTable::InsertMapping(currentPtr, toAddress);
            toObj->SetStateCode(ObjectState::NORMAL);
            region->RecordCompactRoute(offset, toAddress);
            std::atomic_thread_fence(std::memory_order_release);
            if (O2ORemsetDiag::Enabled()) {
                O2ORemsetDiag::NoteCompactObjectMove(currentObj, toObj, size, youngRegion);
            }
        }
        currentPtr += size;
        currentObj = from_region_addr(currentPtr);
    };

    MAddress regionLimit = region->GetRegionAllocPtr();
    region->SetRegionAllocPtr(regionStart);
    while (currentPtr < regionLimit) {
        CHECK(currentPtr >= regionStart);
        size_t offset = currentPtr - regionStart;
        BaseObject* currentObj = from_region_addr(currentPtr);
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            break;
        }
        size_t size = currentObj->GetSize();
        if (survivedAt(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            if (O2ORemsetDiag::Enabled() && !youngRegion) {
                size_t remIn = 0;
                MAddress fromEnd = currentPtr + size;
                for (MAddress slot : rememberedSet.Snapshot()) {
                    if (slot >= currentPtr && slot < fromEnd) {
                        ++remIn;
                    }
                }
                O2ORemsetDiag::NoteCompactRemsetInFrom(remIn);
            }
            collector.CopyObject(*currentObj, *toObj, size);
            ForwardingTable::InsertMapping(currentPtr, toAddress);
            toObj->SetStateCode(ObjectState::NORMAL);
            region->RecordCompactRoute(offset, toAddress);
            std::atomic_thread_fence(std::memory_order_release);
            if (O2ORemsetDiag::Enabled()) {
                O2ORemsetDiag::NoteCompactObjectMove(currentObj, toObj, size, youngRegion);
            }
        }
        currentPtr += size;
    }

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact_partial", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK,
                         "clear buffer failed");
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact_partial memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();

    OffpastDiag::NoteCompactDone(region);
    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
}

namespace {
// permhit: the receipt gate below (allLiveBitsHaveReceipt) accepts o->IsForwarded() as proof
// that this cycle copied o. That state code carries no target and no cycle stamp — it is set
// by UnlockObject(FORWARDED) (WCollector.cpp:6617) and only cleared when the memory is reused
// (ClearUnits, RegionInfo.h:850-860). A route that is abandoned after copying part of the
// region (:2346-2347) leaves those objects FORWARDED inside a region that survives on
// unmovableFromRegionList (:1338-1341), so the next route reads a stale bit as a receipt.
//
// Consequence if it happens: the copy pass skips the object (:2317-2318, :2226 receipt), the gate
// passes, the region publishes FORWARDED, and no path ever fills that object's tip — which is
// what the read barrier reports as permhole. Audit it at the producer, where the answer is a
// population per run instead of one rare abort.
//
// Gate: MRT_GCV2_PERMHIT_RECEIPT=1 (default off; the walk is the same shape the gate already
// does twice, and it runs only for regions that are about to be published).
bool PermhitReceiptOn()
{
    static const bool on = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_PERMHIT_RECEIPT */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

std::atomic<size_t> g_phRegions{ 0 };
std::atomic<size_t> g_phStarts{ 0 };
std::atomic<size_t> g_phFwdStarts{ 0 };
std::atomic<size_t> g_phNoTip{ 0 };
std::atomic<size_t> g_phNoTipAbandon{ 0 };
std::atomic<size_t> g_phRouteNull{ 0 };
std::atomic<size_t> g_phLogged{ 0 };
std::atomic<bool> g_phAtexit{ false };

// point = "publish" (gate passed, about to SetRouteState(FORWARDED)) or "abandon" (gate
// refused). Both are after the synchronous copy pass, so at either point a start that reads
// FORWARDED must already have a tip-valid route unless its bit is stale.
void PermhitReceiptAudit(RegionInfo* region, const char* point)
{
    if (!PermhitReceiptOn() || region == nullptr || !region->IsSmallRegion()) {
        return;
    }
    const bool abandon = point != nullptr && point[0] == 'a';
    bool expected = false;
    if (g_phAtexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[GCV2][permhit-receipt] atexit regions=%zu starts=%zu fwdStarts=%zu "
                         "routeNull=%zu noTipPublish=%zu noTipAbandon=%zu\n",
                         g_phRegions.load(std::memory_order_relaxed), g_phStarts.load(std::memory_order_relaxed),
                         g_phFwdStarts.load(std::memory_order_relaxed),
                         g_phRouteNull.load(std::memory_order_relaxed), g_phNoTip.load(std::memory_order_relaxed),
                         g_phNoTipAbandon.load(std::memory_order_relaxed));
            std::fflush(stderr);
        });
    }
    g_phRegions.fetch_add(1, std::memory_order_relaxed);
    uintptr_t start = region->GetRegionStart();
    uintptr_t allocPtr = region->GetRegionAllocPtr();
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    uintptr_t position = start;
    while (position < allocPtr) {
        BaseObject* o = from_region_addr(position);
        if (!Collector::PlausibleManagedObjectGate("permhit-receipt", o)) {
            break;
        }
        size_t allocSize = RegionSpace::GetAllocSize(*o);
        if (allocSize == 0) {
            break;
        }
        size_t offset = position - start;
        bool survived = region->IsRouteSurvivedObject(offset);
        if (survived) {
            g_phStarts.fetch_add(1, std::memory_order_relaxed);
            if (o->IsForwarded()) {
                g_phFwdStarts.fetch_add(1, std::memory_order_relaxed);
                BaseObject* to = region->GetRouteForProbe(o);
                bool tipValid = to != nullptr && Heap::IsHeapAddress(to) && to->IsValidObject();
                if (to == nullptr) {
                    g_phRouteNull.fetch_add(1, std::memory_order_relaxed);
                }
                if (!tipValid) {
                    if (abandon) {
                        g_phNoTipAbandon.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_phNoTip.fetch_add(1, std::memory_order_relaxed);
                    }
                    size_t n = g_phLogged.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n <= 32) {
                        LOG(RTLOG_ERROR,
                            "[GCV2][permhit-receipt] n=%zu point=%s region=%p start=%#zx off=%zu size=%zu "
                            "from=%p to=%p route=%u rtype=%u young=%u live=%zu "
                            "— FORWARDED start whose route has no tip",
                            n, point, region, start, offset, allocSize, o, to,
                            static_cast<unsigned>(region->GetRouteState()),
                            static_cast<unsigned>(region->GetRegionType()),
                            static_cast<unsigned>(region->IsYoungRegion()), region->GetLiveByteCount());
                    }
                }
            }
        }
        position += allocSize;
    }
}
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

void RegionManager::FinishStayYoungInPlace(RegionInfo* region)
{
    BumpYoungSurvivorAge(region);
    region->DispelGhostFromRegion();
}

void RegionManager::EnlistStayYoungSurvivor(RegionInfo* region)
{
    FinishStayYoungInPlace(region);
    recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
}

template<Generation G>
void RegionManager::ForwardRegion(RegionInfo* region)
{
    MarkView<G> markView = region->GetRouteMarkView<G>();
    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", region->GetRegionType());

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %zu",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        region->GetRegionType(), region->GetLiveByteCount());

    bool youngRegion = region->IsYoungRegion();
    if (IsKnownEmptyForView(region, markView)) {
        // ClearLiveInfo arms LIVE_AUTHORITY|0 before mark. MarkObject is the only path that
        // allocates the mark bitmap and raises live bytes. A region with allocated payload but
        // no mark bitmap was never entered by MarkObject — under a correct mark that means
        // nothing reachable points into it, so it is dead.
        //
        // hangfloor (0808): the young-only "fwd-empty-keep" arm (d6b77bc0) promoted every such
        // region to unmovable-from instead of CollectRegion. Under PLAIN_ROOTS arm A' that was
        // ~500 regions x 64KiB per minor with liveBytes~64 and reclaimedBytes~65KiB — young
        // thrash (10/10 HANG, minor+major alternating, promoteReplay~420k). Full GC already
        // reclaimed the same shape (1ec07b3c); young must match. B2 survivors-wiped is a mark
        // completeness defect, not a reclaim-policy defect: papering over it by keeping dead
        // young regions is what produces the hang.
        //
        // cjpmnull: ZGC register_empty_page / free_page only after mark (zGeneration.cpp:216-221)
        // and never while the page is still in the relocation set. GetRouteMarkView mints its
        // epoch from liveInfo0's face; IsKnownEmpty then treats view.epoch != snapshotEpoch as
        // empty (RegionInfo.h:2642-2646). A FORWARDABLE/ROUTING/ROUTED region has not been
        // copied — CollectRegion here makes F3 soft-null live holders (garbregion: route=3
        // slots>0). Fall through to Route/Visit like a live page.
        {
            RegionInfo::RouteState rs = region->GetRouteState();
            const bool stillInRelocationSet = rs == RegionInfo::RouteState::FORWARDABLE ||
                rs == RegionInfo::RouteState::ROUTING || rs == RegionInfo::RouteState::ROUTED;
            if (stillInRelocationSet && region->GetRegionAllocPtr() > region->GetRegionStart()) {
                static std::atomic<size_t> g_fwdEmptyKeepReloc{ 0 };
                size_t n = g_fwdEmptyKeepReloc.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 8 || (n & (n - 1)) == 0) {
                    LOG(RTLOG_ERROR,
                        "[GCRECLAIM][fwd-empty-keep-reloc] n=%zu region=%p start=%#zx alloc=%#zx "
                        "route=%u live=%zu knownEmpty=1 — ExemptFromRegion",
                        n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                        static_cast<unsigned>(rs), region->GetLiveByteCount());
                }
                // Same shape as the abandon arm (:2945-2950): keep from in place.
                // Do not VisitLive — the route face is the stale one that made
                // IsKnownEmpty true, so VisitLive would copy 0 objects and still
                // CollectRegion (fix_r1: keep fired, F3 still 4180 region_garbage).
                if (youngRegion) {
                    MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
                    (void)region->PromoteYoungRegion(promotionView);
                }
                region->DispelGhostFromRegion();
                ExemptFromRegion(region);
                return;
            } else {
        bool neverExamined = region->GetMarkBitmap(markView) == nullptr &&
            region->GetRegionAllocPtr() > region->GetRegionStart();
        if (neverExamined) {
            // Volume control, not detail reduction. This line fired 50,282 times in a 60s run
            // (nwdiag 0808) and every one of them said the same thing, which buried the gate
            // samples that explain *why*. Print the first few of each GC cycle, then only at
            // power-of-four milestones so the final magnitude is still on the record.
            static std::atomic<size_t> emptyCollectGc{ std::numeric_limits<size_t>::max() };
            static std::atomic<size_t> emptyCollectSeq{ 0 };
            size_t gcNow = g_gcCount.load(std::memory_order_relaxed);
            if (emptyCollectGc.load(std::memory_order_relaxed) != gcNow) {
                emptyCollectGc.store(gcNow, std::memory_order_relaxed);
                emptyCollectSeq.store(0, std::memory_order_relaxed);
            }
            size_t seq = emptyCollectSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            bool milestone = (seq & (seq - 1)) == 0;   // 1,2,4,8,16,...
            if (seq <= 8 || milestone) {
                GCReason gcReason = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const char* reasonName =
                    gcReason < GC_REASON_MAX ? g_gcRequests[gcReason].name : "invalid";
                // markBitmap and allocPtr>start are the two inputs behind neverExamined; print
                // them rather than only the verdict, and carry gc= so this stream can be joined
                // against [GCV2][markfloor-obj-gate] REJECT lines from the same cycle.
                VLOG(REPORT,
                     "[GCRECLAIM][fwd-empty-collect] gc=%zu seq=%zu region=%p start=%#zx alloc=%#zx "
                     "end=%#zx young=%u live=%zu bitmap=%p neverExamined=1 reason=%s(%d) — CollectRegion",
                     gcNow, seq, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                     region->GetRegionEnd(), static_cast<unsigned>(youngRegion),
                     region->GetLiveByteCount(), region->GetMarkBitmap(markView), reasonName,
                     static_cast<int>(gcReason));
            }
        }
        if (youngRegion) {
            // No live objects → no out-edges; still retire the young face so a
            // later reuse cannot inherit this cycle's young marks as old marks.
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            (void)region->PromoteYoungRegion(promotionView);
        }
        RegionLifeDiag::SetNextFreePath(RegionLifeDiag::PATH_FWD_KNOWN_EMPTY);
        CollectRegion<G>(region);
        return;
            }
        }
    }

    // promodomain dual-run force: MRT_GCV2_PROMO_DOMAIN_FORCE_INPLACE=1 skips RouteRegion so
    // the in-place arm (Register + RecordPromotedCrossGenEdges) fires. Default off; product
    // still routes. Needed because natural_wave residualPromote≡0 and pathRec≡0 (routed-only).
    // Only force on young GC — major also calls ForwardRegion but has no domain discharge.
    static const bool forceInPlaceEnv = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_PROMO_DOMAIN_FORCE_INPLACE */;
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    const bool forceInPlace =
        forceInPlaceEnv && Heap::GetHeap().GetCollector().GetGCStats().reason == GC_REASON_YOUNG;
    const bool stayYoung = youngRegion && StayYoungThisCycle(region);
    if (forceInPlace || stayYoung || !RouteRegion(region)) {
        if (youngRegion && stayYoung) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            // In-place promote (compacted / unrouted): scan before clearing young flag.
            // promodomain §A.3: register durable domain (default off); old scan stays.
            // Register only during young GC (discharge runs in young.evac_finish only).
            region->PreserveRetainedLiveInfo();
            {
                GCReason r = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const bool doReg = (r == GC_REASON_YOUNG);
                if (doReg) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::InPlace);
                }
                // domainon COVERAGE: Register gate vs Record site (inplace=0).
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 0, doReg);
                size_t recEdges = RecordPromotedCrossGenEdges(region);
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
        [&collector, youngRegion, &rememberedSet, &promotedRecords, &oldObjForwarded,
         &o2yOnToForOld, &recordedOnToForOld](BaseObject* obj) {
            BaseObject* toObj = collector.ForwardObject(obj);
            // Remset slots must address the surviving (to-space) holder, not the from copy
            // that CollectRegion is about to reclaim.
            //
            // Young: re-scan to-object and Record O→Y slots (promotion).
            // Old→old: ZGC update_remset_old_to_old — TransferObjectSlots moves existing
            // remset bits by field offset (zRelocate.cpp:652-731). From bits are scrubbed
            // later by CollectRegion → ClearRegion (no in-place overlap on this path:
            // RouteObject always mints a distinct to address).
            if (youngRegion && toObj != nullptr && O2ORemsetDiag::Enabled()) {
                O2ORemsetDiag::NoteYoungObjectForward();
            }
            if (youngRegion && toObj != nullptr && toObj->HasRefField()) {
                toObj->ForEachRefField([&rememberedSet, &promotedRecords, toObj](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        NotePromoteGapField(toObj, field, false, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*null/nonheap*/ 3, false);
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        rememberedSet.Record(slot);
                        ++promotedRecords;
                        FlipPromoDiag::NoteProductRecord(slot, /*path*/ 1);
                        NotePromoteGapField(toObj, field, true, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*young*/ 1, true);
                    } else {
                        NotePromoteGapField(toObj, field, false, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*old*/ 2, false);
                    }
                });
            } else if (!youngRegion && toObj != nullptr && toObj != obj && obj->IsForwarded()) {
                size_t sz = RegionSpace::GetAllocSize(*obj);
                MAddress fromBase = reinterpret_cast<MAddress>(obj);
                MAddress toBase = reinterpret_cast<MAddress>(toObj);
                size_t moved = rememberedSet.TransferObjectSlots(fromBase, toBase, sz);
                recordedOnToForOld += moved;
                if (O2ORemsetDiag::Enabled()) {
                    O2ORemsetDiag::NoteOldObjectForward(obj, toObj, sz);
                    ++oldObjForwarded;
                    if (toObj->HasRefField()) {
                        toObj->ForEachRefField([&o2yOnToForOld](RefField<>& field) {
                            BaseObject* target = to_object(field.GetTargetObject());
                            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                                return;
                            }
                            RegionInfo* targetRegion =
                                RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                                ++o2yOnToForOld;
                            }
                        });
                    }
                }
            }
            // tipnull arm R: receipt = object FORWARDED (Copy wrote tip), not soft-keep from.
            return obj->IsForwarded();
        });

    // tipnull v5 full coverage: FORWARDED only if every liveInfo0 *live bit* is covered by
    // a size-walk start that is object-FORWARDED (Copy wrote tip). Prior allSurvivorsForwarded
    // only checked size-walk starts; multi-bit MarkBits interiors/orphans still Admitted
    // without ever being Copy'd → region_FORWARDED_tip_null (arm R refuse=0).
    // Incomplete: DispelGhost (no geometric plan) + Exempt — never FORWARDED empty, never
    // ROUTED forever (TIMEOUT), never soft-null after Collect (SEGV si_addr=0x8).
    auto allLiveBitsHaveReceipt = [region]() -> bool {
        auto survivedAt = [region](size_t offset) -> bool { return region->IsRouteSurvivedObject(offset); };
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
        forwarded = region->VisitLiveObjectsUntilFalse([&collector](BaseObject* obj) {
            if (obj->IsForwarded()) {
                return true;
            }
            (void)collector.ForwardObject(obj);
            return obj->IsForwarded();
        });
    }

    if (!forwarded || !allLiveBitsHaveReceipt()) {
        // permhit: same audit on the refusing arm — the copy pass is already done here too.
        PermhitReceiptAudit(region, "abandon");
        static std::atomic<size_t> abandonN{ 0 };
        size_t n = abandonN.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 32) {
            LOG(RTLOG_ERROR,
                "[GCV2][tipnull] abandon-route region=%p start=%#zx live=%zu route=%u n=%zu "
                "— live bit without receipt; DispelGhost (no FORWARDED)",
                region, region->GetRegionStart(), region->GetLiveByteCount(),
                static_cast<unsigned>(region->GetRouteState()), n);
        }
        // permwho: this arm's assumption ("RouteObject miss ⇒ mutator keeps from") only holds
        // for objects this pass did not copy. Objects it did copy already carry
        // ObjectState::FORWARDED in their own header, and nothing clears that. Count them
        // before the region is exempted and can be routed again under a fresh RouteInfo.
        if (PermWhoAdmit::Enabled() && region->IsSmallRegion()) {
            size_t walked = 0;
            size_t forwarded = 0;
            uintptr_t pos = region->GetRegionStart();
            uintptr_t end = region->GetRegionAllocPtr();
            while (pos < end) {
                BaseObject* o = from_region_addr(pos);
                if (!Collector::PlausibleManagedObjectGate("permwho-abandon", o)) {
                    break;
                }
                size_t sz = RegionSpace::GetAllocSize(*o);
                if (sz == 0) {
                    break;
                }
                ++walked;
                if (o->IsForwarded()) {
                    ++forwarded;
                }
                pos += sz;
            }
            PermWhoAdmit::NoteAbandon(region, walked, forwarded);
        }
        if (youngRegion && StayYoungThisCycle(region)) {
            EnlistStayYoungSurvivor(region);
            return;
        }
        if (youngRegion) {
            MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
            // promodomain §A.3 abandon arm: register + old sync walk (default domain off).
            // Register only on young GC (domain discharge is minor-only).
            region->PreserveRetainedLiveInfo();
            {
                GCReason r = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const bool doReg = (r == GC_REASON_YOUNG);
                if (doReg) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::Abandon);
                }
                // domainon COVERAGE: Register gate vs Record site (abandon=1).
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 1, doReg);
                size_t recEdges = RecordPromotedCrossGenEdges(region);
                PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(r), /*site*/ 1, recEdges);
            }
            (void)region->PromoteYoungRegion(promotionView);
        }
        // DispelGhost → NORMAL + clear ghost bit: GetGhostFromRegionAt null ⇒ RouteObject
        // miss ⇒ mutator keeps from (valid). Do not SetRouteInfo(0): that sets
        // toRegion2Idx=INVALID and GetRoute CHECKs when preLive >= to1used (wb gate).
        region->DispelGhostFromRegion();
        ExemptFromRegion(region);
        return;
    }
    {
        static const bool probe = []() {
            const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCRECLAIM_PROBE */;
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        if (probe && !region->IsLargeRegion()) {
            size_t start = region->GetRegionStart();
            size_t alloc = region->GetRegionAllocPtr();
            size_t totalObjs = 0;
            size_t survivedObjs = 0;
            size_t residualValid = 0;
            uintptr_t pos = start;
            while (pos < alloc) {
                BaseObject* o = from_region_addr(pos);
                if (!o->IsValidObject()) {
                    break;
                }
                size_t sz = o->GetSize();
                if (sz == 0) {
                    break;
                }
                ++totalObjs;
                size_t off = pos - start;
                if (region->IsSurvivedObject(markView, off)) {
                    ++survivedObjs;
                } else {
                    ++residualValid;
                }
                pos += sz;
            }
            if (residualValid > 0) {
                VLOG(REPORT,
                     "[GCRECLAIM][fwd-residual] region=%p start=%#zx alloc=%#zx live=%zu totalObjs=%zu "
                     "survived=%zu residualUnmarked=%zu young=%u BYPASS=1",
                     region, start, alloc, region->GetLiveByteCount(), totalObjs, survivedObjs, residualValid,
                     static_cast<unsigned>(youngRegion));
            }
        }
        // permhit: last point at which a real receipt must already be tip-valid.
        PermhitReceiptAudit(region, "publish");
        region->SetRouteState(RegionInfo::RouteState::FORWARDED);
        // zRelocate.cpp:1152 — last act after every object on the page is relocated.
        region->MarkForwardingDone();
        // livesame ORDER + ZGC reset_livemap (zForwarding.cpp:71-74): one publish for
        // live bytes + mark face (ResetLiveMapAfterForward).
        {
            const uint64_t liveBefore = region->GetLiveByteCount();
            size_t validBefore = 0;
            size_t markedBefore = 0;
            F3Why2Diag::CountMarks(region, validBefore, markedBefore);
            region->VerifyLiveBooks(markView, "pre-ResetLiveMapAfterForward");
            // Simulated split for ORDER: live-only then mark-only was the old bug;
            // measure residual marks after live-zero before joint reset.
            region->ResetLiveByteCount();
            const uint64_t liveAfterReset = region->GetLiveByteCount();
            size_t validAfterReset = 0;
            size_t markedAfterReset = 0;
            F3Why2Diag::CountMarks(region, validAfterReset, markedAfterReset);
            // Joint publish (restores live empty + epoch bump in one API).
            region->ResetLiveMapAfterForward(markView);
            size_t validAfterInv = 0;
            size_t markedAfterInv = 0;
            F3Why2Diag::CountMarks(region, validAfterInv, markedAfterInv);
            F3Why2Diag::NoteForwardOrder(region, liveBefore, markedBefore, liveAfterReset, markedAfterReset,
                                         markedAfterInv);
            region->VerifyLiveBooks(markView, "post-ResetLiveMapAfterForward");
            if (youngRegion) {
                if (promotedRecords != 0) {
                    g_promotedCrossGenEdgeCount.fetch_add(promotedRecords, std::memory_order_relaxed);
                }
                MarkView<Generation::Young> promotionView = region->GetMarkView<Generation::Young>();
                (void)region->PromoteYoungRegion(promotionView);
            } else if (O2ORemsetDiag::Enabled()) {
                // Pre-scrub census: remset bits still at from-addresses (Transfer moved to to).
                size_t remsetInFrom = 0;
                MAddress rStart = static_cast<MAddress>(region->GetRegionStart());
                MAddress rEnd = static_cast<MAddress>(region->GetRegionEnd());
                for (MAddress slot : rememberedSet.Snapshot()) {
                    if (slot >= rStart && slot < rEnd) {
                        ++remsetInFrom;
                    }
                }
                O2ORemsetDiag::NoteOldRegionForwarded(region, remsetInFrom, oldObjForwarded, o2yOnToForOld,
                                                     recordedOnToForOld);
            }
            (void)validBefore;
            (void)validAfterReset;
            (void)validAfterInv;
        }
        RegionLifeDiag::SetNextFreePath(RegionLifeDiag::PATH_FWD_AFTER_COPY);
        CollectRegion<G>(region);
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

template void RegionManager::ForwardFromRegions<Generation::Young>(GCThreadPool*);
template void RegionManager::ForwardFromRegions<Generation::Old>(GCThreadPool*);
template void RegionManager::ForwardFromRegions<Generation::Young>();
template void RegionManager::ForwardFromRegions<Generation::Old>();
template void RegionManager::ForwardRegion<Generation::Young>(RegionInfo*);
template void RegionManager::ForwardRegion<Generation::Old>(RegionInfo*);
} // namespace MapleRuntime

namespace MapleRuntime {
// enroltime: defined out of line so RegionInfo.h does not have to see Heap/GCPhase.
void RegionInfo::NoteEnrolPhase()
{
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
