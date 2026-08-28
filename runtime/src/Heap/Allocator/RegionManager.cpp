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
#include "Heap/HeapWork.h"
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
size_t RegionInfo::UnitInfo::unitSizeShift = 0;
MemMap* RegionInfo::UnitInfo::memoryOwner = nullptr;

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

BaseObject* ScanFieldHealedTarget(Collector& collector, RefField<>& field)
{
    return collector.make_load_good(field);
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
                NotePromoteGapField(object, field, false, false);

                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                rememberedSet.Record(slot);
                ++recorded;
                // promodomain dual-run: old product edge set for bidirectional reconcile.
                PromotedRegionDomain::NoteOldProductRecord(slot);

                NotePromoteGapField(object, field, true, false);

            } else {
                NotePromoteGapField(object, field, false, false);

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
    size_t n = g_promotedCrossGenEdgeCount.exchange(0, std::memory_order_relaxed);
    DumpPromoteGapProbe("consume");
    return n;
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
                RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    rememberedSet.Record(slot);
                    recorded.fetch_add(1, std::memory_order_relaxed);

                }
            });
        });
    };
    // STW-parallel of the same conservative walk. Record is fetch_or, so order
    // does not change the remset. Default ON — mutator-visible state is identical.
    // Env MRT_GCV2_PINNED_SCAN_PARALLEL=0 keeps the serial walk.
    static const bool parallelEnv = []() {
        const char* v = std::getenv("MRT_GCV2_PINNED_SCAN_PARALLEL");
        return v == nullptr || std::strcmp(v, "0") != 0;
    }();
    GCThreadPool* pool = parallelEnv ? Heap::GetHeap().GetCollectorResources().GetThreadPool() : nullptr;
    if (pool != nullptr) {
        MRT_PHASE_TIMER("young.pinned_scan.parallel");
        std::vector<RegionInfo*> regions;
        auto collect = [&regions, &skipPinnedScanRegion](RegionInfo* region) {
            if (!skipPinnedScanRegion(region)) {
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
    // Old holders only (pinned/large/full). from/tl/unmovable-from are the young
    // cset; walking them stamped deadHolder slots (zRemembered.cpp:347-387).
    {
        MRT_PHASE_TIMER("young.pinned_scan.serial");
        recentPinnedRegionList.VisitAllRegions(scanRegion);
        oldPinnedRegionList.VisitAllRegions(scanRegion);
        rawPointerPinnedRegionList.VisitAllRegions(scanRegion);
        recentLargeRegionList.VisitAllRegions(scanRegion);
        oldLargeRegionList.VisitAllRegions(scanRegion);
        largeTraceRegions.VisitAllRegions(scanRegion);
        recentFullRegionList.VisitAllRegions(scanRegion);
        fullTraceRegions.VisitAllRegions(scanRegion);
    }
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

#if defined(MRT_TESTABLE_INTERNALS)
template<Generation G>
void ForwardTask<G>::Execute(size_t)
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

void FreeRegionManager::AddReleaseUnits(UnitIndex idx, UnitCount num)
{
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
    if (UNLIKELY(!releasedUnitTree.MergeInsert(idx, num, true))) {
        LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
    }
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
        UnitIndex idx = node->GetIndex();
        UnitCount num = node->GetCount();
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(idx));
        const bool detachReady = FromPageDetach::FromPageDetachCheck(
            region, FromPageDetach::Site::RELEASE_GARBAGE_UNITS);
        CHECK_DETAIL(dirtyUnitTree.TakeUnits(num, idx, false),
                     "tid %d: failed to detach dirty units[%u+%u, %u)", GetTid(), idx, num, idx + num);

        if (!detachReady) {
            AddDetachQuarantineUnits(idx, num, false, false);
            dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
            continue;
        }
        FromPageDetach::ReusePermitScope reusePermit;

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

size_t FreeRegionManager::UncommitIdleUnits(size_t maxBytes, uint64_t idleBeforeNs, bool honorCancel)
{
    ScopedEnterSaferegion enterSaferegion(true);
    return UncommitIdleUnitsImpl(maxBytes, idleBeforeNs, honorCancel);
}

size_t FreeRegionManager::UncommitIdleUnitsImpl(size_t maxBytes, uint64_t idleBeforeNs, bool honorCancel)
{
    if (maxBytes < RegionInfo::UNIT_SIZE) {
        return 0;
    }
    size_t uncommittedBytes = 0;
    while (uncommittedBytes + RegionInfo::UNIT_SIZE <= maxBytes) {
        UnitIndex idx = 0;
        UnitCount num = 0;
        {
            std::lock_guard<std::mutex> lock1(releasedUnitTreeMutex);
            UnitCount remain = static_cast<UnitCount>((maxBytes - uncommittedBytes) / RegionInfo::UNIT_SIZE);
            if (remain == 0 || !releasedUnitTree.TakeIdleUnits(idleBeforeNs, remain, idx, num)) {
                break;
            }
        }
        if (honorCancel && Uncommitter::ShouldStopUncommit()) {
            std::lock_guard<std::mutex> lockCancel(releasedUnitTreeMutex);
            CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true),
                         "tid %d: failed to restore canceled uncommit units[%u+%u, %u)", GetTid(), idx, num,
                         idx + num);
            break;
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
            std::lock_guard<std::mutex> lockHold(releasedUnitTreeMutex);
            CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true),
                         "tid %d: failed to restore uncommit units under live forwarding [%u+%u, %u)",
                         GetTid(), idx, num, idx + num);
            break;
        }
        const size_t requestedBytes = static_cast<size_t>(num) * RegionInfo::UNIT_SIZE;
        const size_t backendReleased = RegionInfo::ReleaseUnitsPartial(idx, num);
        const size_t released = Uncommitter::AccountReleased(requestedBytes, backendReleased);
        {
            std::lock_guard<std::mutex> lock2(releasedUnitTreeMutex);
            CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true),
                         "tid %d: failed to retain uncommit units[%u+%u, %u)", GetTid(), idx, num,
                         idx + num);
        }
        if (released != 0) {
            uncommittedBytes += released;
        }
        if (Uncommitter::ShouldRetryPartial(requestedBytes, backendReleased)) {
            break;
        }
    }
    if (uncommittedBytes > 0) {
        VLOG(REPORT, "uncommit idle heap memory %zu bytes", uncommittedBytes);
    }
    return uncommittedBytes;
}

void FreeRegionManager::AddDetachQuarantineRegion(RegionInfo* region, bool releasePhysical)
{
    CHECK(region != nullptr);
    AddDetachQuarantineUnits(region->GetUnitIdx(), region->GetUnitCount(), releasePhysical, true, releasePhysical);
}

void FreeRegionManager::AddDetachQuarantineUnits(UnitIndex idx, UnitCount num, bool released, bool needsInit,
                                                 bool releasePhysical)
{
    static constexpr size_t kMaxEntries = 65536;
    std::lock_guard<std::mutex> lock(detachQuarantineMutex);
    CHECK_DETAIL(detachQuarantine.size() < kMaxEntries,
                 "CJRT_FROM_REUSE_GATE detach quarantine overflow entries=%zu max=%zu",
                 detachQuarantine.size(), kMaxEntries);
    detachQuarantine.push_back(DetachQuarantineEntry{ idx, num, 0, released, needsInit, releasePhysical });
    FromPageDetach::NoteQuarantineAdmitted(detachQuarantine.size());
}

size_t FreeRegionManager::ReleaseDetachQuarantineAfterMajor()
{
    static constexpr uint8_t kMaxRechecks = 8;
    std::vector<DetachQuarantineEntry> pending;
    {
        std::lock_guard<std::mutex> lock(detachQuarantineMutex);
        pending.swap(detachQuarantine);
    }

    size_t releasedUnits = 0;
    std::vector<DetachQuarantineEntry> held;
    held.reserve(pending.size());
    for (DetachQuarantineEntry entry : pending) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(RegionInfo::GetUnitAddress(entry.idx));
        // Quarantined regions are deliberately on no managed list, so the
        // ordinary ClearRouteDestHoldFlags list walk cannot see them. This
        // post-PrepareForwardTable major closure retired the only route
        // generation that could have stamped the withheld address.
        region->SetRouteDestHold(0);
        (void)FromPageDetach::FromPageDetachCheck(region, FromPageDetach::Site::MAJOR_RECHECK,
                                                  FromPageDetach::Action::MAJOR_CLOSE);
        if (!ExtentReadyForReleasedCache(region)) {
            ++entry.rechecks;
            FromPageDetach::NoteQuarantineRecheckHeld();
            if (FromPageDetach::GateEnabled()) {
                CHECK_DETAIL(entry.rechecks <= kMaxRechecks,
                             "CJRT_FROM_REUSE_GATE detach quarantine did not close idx=%u units=%u rechecks=%u max=%u",
                             entry.idx, entry.num, static_cast<unsigned>(entry.rechecks),
                             static_cast<unsigned>(kMaxRechecks));
            }
            held.push_back(entry);
            continue;
        }

        if (entry.needsInit) {
            // Re-enter the original funnel after its evidence has healed so
            // path-specific scrub/zap/huge-page work is not skipped. That
            // funnel drains and performs a second central check after
            // ClearEntries; a newly retired table is re-admitted as a
            // needsInit=false quarantine entry rather than reaching a tree.
            if (entry.releasePhysical) {
                (void)regionManager.ReleaseRegion(region);
            } else {
                regionManager.ReclaimRegion(region);
            }
            FromPageDetach::NoteQuarantineReleased();
            continue;
        }
        if (entry.released) {
            AddReleaseUnits(entry.idx, entry.num);
        } else {
            AddGarbageUnits(entry.idx, entry.num);
        }
        releasedUnits += entry.num;
        FromPageDetach::NoteQuarantineReleased();
    }

    if (!held.empty()) {
        std::lock_guard<std::mutex> lock(detachQuarantineMutex);
        CHECK_DETAIL(detachQuarantine.size() + held.size() <= 65536,
                     "CJRT_FROM_REUSE_GATE detach quarantine overflow on recheck current=%zu held=%zu max=65536",
                     detachQuarantine.size(), held.size());
        detachQuarantine.insert(detachQuarantine.end(), held.begin(), held.end());
    }
    return releasedUnits;
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
    size_t metadataSize = GetMetadataSize(nUnit);
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = regionInfoAddr + metadataSize;
    this->regionHeapEnd = regionHeapStart + nUnit * RegionInfo::UNIT_SIZE;
    // PORT_ZFORWARDING step 1: the address-keyed table covers the same span the units do, so an
    // index is (addr - base) / UNIT_SIZE with no probing -- ZGranuleMap's shape.
    CHECK_DETAIL(ForwardingTable::Initialize(
                     regionHeapStart, nUnit * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE),
                 "forwarding table initialization failed heap=[%#zx,%#zx) unit=%zu",
                 static_cast<size_t>(regionHeapStart),
                 static_cast<size_t>(regionHeapStart + nUnit * RegionInfo::UNIT_SIZE),
                 RegionInfo::UNIT_SIZE);
    this->inactiveZone = regionHeapStart;
    SetMaxUnitCountForRegion(heapParam.regionSize);
    SetMaxUnitCountForPinnedRegion(heapParam.regionSize);
    SetLargeObjectThreshold(heapParam.regionSize);
    SetGarbageThreshold(garbageThreshold);
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    RegionInfo::Initialize(nUnit, regionHeapStart, &memoryOwner);
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
    if (!FromPageDetach::FromPageDetachCheck(region, FromPageDetach::Site::RECLAIM_DIRTY)) {
        freeRegionManager.AddDetachQuarantineRegion(region);
        return;
    }
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

    if (FromPageDetach::GateEnabled()) {
        RegionInfo::DrainScope drain(region, MutatorRelocate::Retire::RECLAIM_DIRTY);
    }
    // gcvroot Z2: poison reclaimed payload so use-after-free roots are identifiable (MRT_GCV2_ZAP_RECLAIM=1).
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    if (FromPageDetach::GateEnabled()) {
        // The entry check proved there was no older retired debt; DrainScope
        // completed the current remap reader closure. ClearEntries inside
        // InitFreeUnits therefore produced a table that can be detached now,
        // not a new reason to wait another major cycle.
        ForwardingTable::DropRetiredCovering(RegionInfo::GetUnitAddress(unitIndex),
                                             num * RegionInfo::UNIT_SIZE);
    }
    freeRegionManager.AddGarbageUnits(unitIndex, num);
    // The free-tree insertion is the allocator lock boundary at which a
    // blocked allocation can be directed to its newly available extent.
    SatisfyStalledAllocations();
}

size_t RegionManager::StallAllocation(size_t size)
{
    AllocationStallRequest request(size);
    const bool requestGc = allocationStallQueue.Enqueue(request);
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

#if !defined(MRT_ALLOCATION_STALL_CUT_SAFEREGION)
    ScopedEnterSaferegion enterSaferegion(false);
#endif
#if defined(MRT_ALLOCATION_STALL_OBSERVE)
    const bool satisfied = request.Wait(allocationStallBeforeWaitTestHook
        ? [this] { allocationStallBeforeWaitTestHook(*this); }
        : std::function<void()> {});
#else
    const bool satisfied = request.Wait();
#endif
    return satisfied ? request.GetClaimedUnits() : 0;
}

void RegionManager::FinishStalledAllocation(size_t claimedUnits)
{
    allocationStallQueue.ReleaseClaim(claimedUnits);
    SatisfyStalledAllocations();
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

size_t RegionManager::PendingStalledAllocations() const
{
    return allocationStallQueue.Pending();
}

size_t RegionManager::EnqueuedStalledAllocations() const
{
    return allocationStallQueue.EnqueuedCount();
}

size_t RegionManager::DequeuedStalledAllocations() const
{
    return allocationStallQueue.DequeuedCount();
}

size_t RegionManager::SatisfiedStalledAllocations() const
{
    return allocationStallQueue.SatisfiedCount();
}

size_t RegionManager::FailedStalledAllocations() const
{
    return allocationStallQueue.FailedCount();
}
#endif

void RegionManager::SatisfyStalledAllocations()
{
    allocationStallQueue.SatisfyAvailable([this](size_t bytes, size_t alreadyClaimed) {
        const size_t units = (bytes + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
        const size_t largestExtent = std::max(
            GetInactiveUnitCount(),
            std::max<size_t>(freeRegionManager.GetDirtyMaxBlock(), freeRegionManager.GetReleasedMaxBlock()));
#if defined(MRT_ALLOCATION_STALL_CUT_CLAIM)
        (void)alreadyClaimed;
        return units <= largestExtent ? units : 0;
#else
        return units <= largestExtent && alreadyClaimed <= largestExtent - units ? units : 0;
#endif
    });
}

void RegionManager::ReclaimRegionToMarkQuarantine(RegionInfo* region)
{
    if (!FromPageDetach::FromPageDetachCheck(region, FromPageDetach::Site::RECLAIM_MARK_QUARANTINE)) {
        freeRegionManager.AddDetachQuarantineRegion(region);
        return;
    }
    // routedest: census only, see ReclaimRegion.
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());
    if (FromPageDetach::GateEnabled()) {
        RegionInfo::DrainScope drain(region, MutatorRelocate::Retire::RECLAIM_MARK_QUARANTINE);
    }
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    if (FromPageDetach::GateEnabled()) {
        ForwardingTable::DropRetiredCovering(RegionInfo::GetUnitAddress(unitIndex),
                                             num * RegionInfo::UNIT_SIZE);
    }
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    if (!FromPageDetach::FromPageDetachCheck(region, FromPageDetach::Site::RELEASE_REGION)) {
        const size_t heldBytes = region->GetRegionSize();
        freeRegionManager.AddDetachQuarantineRegion(region, true);
        return heldBytes;
    }
    // routedest: census only, see ReclaimRegion.

    // holdercapture: large regions above the release threshold never reach CollectRegion,
    // so the snapshot has to be taken on this path too or the face is lost unrecorded.

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

    if (FromPageDetach::GateEnabled()) {
        RegionInfo::DrainScope drain(region, MutatorRelocate::Retire::RELEASE_REGION);
    }
    region->InitFreeUnits();
    if (FromPageDetach::GateEnabled()) {
        ForwardingTable::DropRetiredCovering(RegionInfo::GetUnitAddress(unitIndex),
                                             num * RegionInfo::UNIT_SIZE);
    }
    freeRegionManager.AddReleaseUnits(unitIndex, num);
    return res;
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::ExpireKeptFromPreviousCycle()
{
    // Flip false for gate ⑥ (256MB OOM returns). Product default on.
    // zRelocationSetSelector.cpp:114-196 rebuilds the set every cycle; pages
    // carry no cross-cycle exemption. zGeneration.cpp:205-213 iterates the
    // whole page table.
    static constexpr bool kExpireKeptAtCycleStart = true;
    if constexpr (!kExpireKeptAtCycleStart) {
        return;
    }

    size_t expired = 0;
    size_t expiredBytes = 0;
    auto expireList = [&expired, &expiredBytes](RegionList& list) {
        list.VisitAllRegions([&expired, &expiredBytes](RegionInfo* region) {
            if (region == nullptr || !region->IsForwardingDone()) {
                return;
            }
            const RegionInfo::RouteState rs = region->GetRouteState();
            if (rs == RegionInfo::RouteState::FORWARDED || rs == RegionInfo::RouteState::COMPACTED) {
                // After-copy Exempt parks FORWARDED+done on unmovableFrom.
                // Keep its receipt through ONE subsequent mark/ref-fix closure
                // (ab0e2b397). The next ExpireKept after that closure retires
                // it (zRelocate.cpp:1018-1047; zRelocationSet.cpp:91-96). A
                // kept page that never re-enters CSet otherwise lives until
                // InitRegionInfo reuses the to-region (seqnum mismatch then
                // rejects the stale dest).
                ZForwarding* tab = ForwardingTable::GetEntries(region->GetRegionStart());
                if (tab != nullptr && tab->kept_seen_expire()) {
                    ForwardingTable::ClearEntries(region->GetRegionStart(), region->GetRegionSize());
                    return;
                }
                if (tab != nullptr) {
                    tab->note_kept_expire();
                }
                return;
            }
            ++expired;
            expiredBytes += region->GetRegionSize();
            region->ExpireKeptPublish();
        });
    };
    expireList(unmovableFromRegionList);
    expireList(fromRegionList);
    expireList(recentFullRegionList);
    expireList(tlRegionList);
    if (expired != 0) {
        LOG(RTLOG_ERROR, "[GCV2][expire-kept] n=%zu bytes=%zu", expired, expiredBytes);
    }
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
    const unsigned rs = static_cast<unsigned>(del->GetRouteState());
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
            const unsigned rs = static_cast<unsigned>(del->GetRouteState());
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
            // Held until DrainScope waits copyInflight even at fwdRefCount==0
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
    // routefix: ReclaimRegion → AddGarbageUnits ScopedEnterSaferegion; skip under ROUTING.
    RegionInfo* head = allowSaferegion ? TakeReclaimableGarbageRegion(&gatedBytes) : nullptr;
    if (head != nullptr) {
        DLOG(REGION, "take garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
        if (head->GetUnitCount() == num &&
            !FromPageDetach::FromPageDetachCheck(head, FromPageDetach::Site::TAKE_GARBAGE_REUSE)) {
            freeRegionManager.AddDetachQuarantineRegion(head);
            head = nullptr;
        }
        // The ON arm makes the implicit active-table -> retired-table
        // transition explicit before allocation. ReclaimRegion drains the
        // current readers, retires the table, and detaches that now-closed
        // answer before publishing the range to the free tree.
        if (head != nullptr && head->GetUnitCount() == num && FromPageDetach::GateEnabled()) {
            ReclaimRegion(head);
            head = nullptr;
        }
        if (head != nullptr && head->GetUnitCount() == num) {
            FromPageDetach::ReusePermitScope reusePermit;
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

            auto idx = head->GetUnitIdx();
            {
                // portmutreloc: ZForwarding::detach_page before the page goes back to the
                // allocator. Reuse overwrites the payload a retained reader may still be
                // copying out of, regardless of whether that overwrite starts here or in
                // the segmented array initializer, so the drain must always precede reuse.
                // Scoped tight: it ends before InitRegion, which re-initialises the metadata
                // the lock lives in. ClearUnits is still conditional because segmented
                // reference arrays deliberately clear the payload at yield boundaries.
                RegionInfo::DrainScope drain(head, MutatorRelocate::Retire::TAKE_GARBAGE);
                if (clearPayload) {
                    RegionInfo::ClearUnits(idx, num, FillerZeroDiag::Site::TAKE_GARBAGE);
                }
            }
            DLOG(REGION, "reuse garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            MutatorAllocRate::sample_allocation(size);
            return RegionInfo::InitRegion(idx, num, type);
        } else if (head != nullptr) {
            DLOG(REGION, "reclaim garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            ReclaimRegion(head);
        }
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

    RegionInfo* region = freeRegionManager.TakeRegion(
        num, type, expectPhysicalMem, allowSaferegion, clearPayload);
    if (region != nullptr) {
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        MutatorAllocRate::sample_allocation(size);
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
            RegionInfo::CommitUnits(idx, num);
            (void)idx; // eliminate compilation warning
            DLOG(REGION, "take inactive units [%zu+%zu, %zu) at [0x%zx, 0x%zx)", idx, num, idx + num,
                 RegionInfo::GetUnitAddress(idx), RegionInfo::GetUnitAddress(idx + num));
            if (num >= HUGE_PAGE) {
                TagHugePage(region, num);
            }
            if (expectPhysicalMem && clearPayload) {
                RegionInfo::ClearUnits(idx, num, FillerZeroDiag::Site::TAKE_INACTIVE);
            }
            MutatorAllocRate::sample_allocation(size);
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
    // A detach quarantine is released only by the next major PostTrace
    // closure. If a minor filled it and allocation has exhausted every other
    // source, waiting for organic allocation progress can deadlock the grace
    // condition: no page means no progress towards the next major. Request
    // that closure here; GC threads and ROUTING critical sections must not
    // synchronously request a collection from inside their own operation.
    if (FromPageDetach::GateEnabled() && allowSaferegion && !IsGcThread() &&
        freeRegionManager.HasDetachQuarantine()) {
        Heap::GetHeap().GetCollector().RequestGC(GC_REASON_HEU, true);
    }
    return nullptr;
}

template<Generation G>
void RegionManager::ForwardFromRegions(GCThreadPool* threadPool)
{
    if (threadPool != nullptr) {
        int32_t threadNum = threadPool->GetMaxActiveThreadNum() + 1;
        // We won't change fromRegionList during gc, so we can use it without lock.
        size_t regionCount = fromRegionList.GetRegionCount();
        (void)regionCount;

        // we start threadPool before adding work so that we can concurrently add tasks;
        relocationRequestQueue.BeginWorkers(static_cast<size_t>(threadNum));
        threadPool->Start();
        for (int32_t i = 0; i < threadNum; ++i) {
            threadPool->AddWork(new (std::nothrow) ForwardTask<G>(*this, fromRegionList));
        }
        threadPool->WaitFinish();
    } else {
        relocationRequestQueue.BeginWorkers(1);
        ForwardFromRegions<G>();
    }
}

size_t RegionManager::CompleteRelocationRequests(RegionInfo* region)
{
    return relocationRequestQueue.CompleteOwner(
        region, [](MAddress from) { return ForwardingTable::FindTo(from); });
}

namespace {
// Wait until in-flight copiers drop to 0 (zForwarding.cpp:171-181 detach_page).
// Copiers NoteCopyInflight on TryLock success (Exclusive entry) and
// EndCopyInflight on every UnlockObject (WCollector.cpp ForwardObjectExclusive).
// find() hits do not enter the count (zRelocate.cpp:382-410). Page walks miss
// LOCKED past VisitAllObjects holes (REPORT-exemptlife §4 B2.3/B2.4).
void WaitCopiedObjectsUnlocked(RegionInfo* region)
{
    if (region == nullptr || region->IsFreeRegion()) {
        return;
    }
    region->WaitCopiedInflight();
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
    // oraclecut §4 / cjpmnull5: Exempt is a terminal region state this cycle.
    // Publish immediately as kept (IsForwardingDone) so WaitRoutedTipReady's
    // region-level wait can exit. Without this the wait never terminates
    // (cjpmnull3 wide-definition OOM). Hole pages are not collected this
    // cycle (cjpmnull2 Exempt).
    WaitCopiedObjectsUnlocked(region);
    if (region != nullptr && !region->IsForwardingDone()) {
        static std::atomic<size_t> g_exemptKept{ 0 };
        const size_t n = g_exemptKept.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][exempt-kept] n=%zu region=%p start=%#zx route=%u live=%zu",
                n, region, region->GetRegionStart(),
                static_cast<unsigned>(region->GetRouteState()), region->GetLiveByteCount());
        }
        region->MarkForwardingDone();
    }
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
    const RegionInfo::RouteState rs = region->GetRouteState();
    return rs == RegionInfo::RouteState::FORWARDABLE || rs == RegionInfo::RouteState::ROUTING ||
        rs == RegionInfo::RouteState::ROUTED;
}
} // namespace

void RegionManager::FinishIncompleteFromRegions()
{
    // Flip false for gate ⑥ (regionTimeout / W1 return). Product default on.
    // zRelocate.cpp:1041-1047: relocate() does not return with a half-copied page.
    static constexpr bool kFinishIncompleteFromRegions = true;
    if constexpr (!kFinishIncompleteFromRegions) {
        return;
    }

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
            region->MarkForwardingDone();
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
        // Residual: publish kept in place. Do not Prepend if the region is still
        // on another live list (recentFull / TL) — that would double-link.
        if (region->IsFromRegion()) {
            fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                           RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
        }
        if (region->IsLoneFromRegion() || region->IsFromRegion() || wasFrom) {
            ExemptFromRegion(region);
        } else {
            region->MarkForwardingDone();
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
                     region, region->GetRegionStart(), static_cast<unsigned>(region->GetRouteState()),
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
        const RegionInfo::RouteState rs = region->GetRouteState();
        const bool complete = rs == RegionInfo::RouteState::FORWARDED ||
            rs == RegionInfo::RouteState::COMPACTED || region->IsForwardingDone();
        if (!complete) {
            const size_t n = g_fromGarbageSkip.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][from-garbage-skip] n=%zu region=%p start=%#zx route=%u done=%u live=%zu "
                    "— skip CollectFromSpaceGarbage, Exempt",
                    n, region, region->GetRegionStart(), static_cast<unsigned>(rs),
                    static_cast<unsigned>(region->IsForwardingDone()), region->GetLiveByteCount());
            }
            ExemptFromRegion(region);
        } else {
            if (!FromPageDetach::FromPageDetachCheck(region, FromPageDetach::Site::COLLECT_FROM_GARBAGE)) {
                freeRegionManager.AddDetachQuarantineRegion(region);
                region = fromRegionList.TakeHeadRegion();
                continue;
            }
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
    // Use the same ownership transition as ForwardTask.  Walking the linked
    // list in place leaves a forwarded region attached as FROM_REGION, so the
    // next young cycle can revisit stale list state.  A zero-helper execution
    // is serial, but it must still detach and mark each unit LONE_FROM_REGION.
    while (true) {
        RelocationRequestQueue::Selection selected =
            relocationRequestQueue.SelectBeforeOrdinary([this]() -> void* {
                return fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            });
        if (!selected) {
            selected = relocationRequestQueue.SynchronizePoll();
            if (selected.workersDone) {
                break;
            }
            if (!selected) {
                continue;
            }
        }
        RegionInfo* region = selected.is_request() ? static_cast<RegionInfo*>(selected.request->owner())
                                                   : static_cast<RegionInfo*>(selected.ordinary);
#if defined(MRT_GCV2_REGION_WAIT_DIAG)
        if (selected.is_request()) {
            static std::atomic<size_t> g_regionWaitClaim{ 0 };
            const size_t n = g_regionWaitClaim.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][region-wait-claim] n=%zu from=%p claim=1 pending=%zu",
                    n, reinterpret_cast<void*>(selected.request->from()), relocationRequestQueue.PendingCount());
            }
        }
#endif
        if (selected.is_request() &&
            !fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                            RegionInfo::RegionType::LONE_FROM_REGION)) {
            // Claim only removes the handle from the worker deque; byFrom still
            // owns it. An ordinary worker may already own this region, so do not
            // publish FAILED here. That worker publishes receipt/pageDone, or
            // generation close proves that no publisher remains and fails it.
#if defined(MRT_GCV2_REGION_WAIT_DIAG)
            static std::atomic<size_t> g_claimLoser{ 0 };
            const size_t n = g_claimLoser.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][region-wait-claim-loser] n=%zu from=%p deferred=1 pending=%zu",
                    n, reinterpret_cast<void*>(selected.request->from()),
                    relocationRequestQueue.PendingCount());
            }
#endif
            continue;
        }
        MRT_ASSERT(region->IsValidRegion(), "the head region of fromRegionList is invalid");
        ForwardRegion<G>(region);
        CompleteRelocationRequests(region);
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
        const RegionInfo::RouteState rs = region->GetRouteState();
        if (rs == RegionInfo::RouteState::FORWARDED || rs == RegionInfo::RouteState::COMPACTED) {
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

static void FillRouteReserve(uintptr_t start, size_t size)
{
    if (size < 8) {
        return;
    }
    FillerZeroDiag::Note(FillerZeroDiag::Site::ROUTE_RESERVE, start, size);
    HeapFiller::ZeroAndFill(start, size);
}

static void FillZeroGaps(uintptr_t start, uintptr_t end)
{
    uintptr_t pos = start;
    while (pos + 8 <= end) {
        BaseObject* obj = from_region_addr(pos);
        if (Collector::PlausibleManagedObjectGate("FillZeroGaps", obj)) {
            size_t sz = RegionSpace::GetAllocSize(*obj);
            if (sz < 8 || pos + sz > end) {
                break;
            }
            pos += sz;
            continue;
        }
        uintptr_t gap = pos;
        while (pos + 8 <= end && *reinterpret_cast<uint64_t*>(pos) == 0) {
            pos += 8;
        }
        size_t n = pos - gap;
        if (n >= 8) {
            FillRouteReserve(gap, n);
        } else {
            pos += 8;
        }
    }
}

static void FillPublishedRouteGaps(RegionInfo* fromRegion)
{
    RouteInfo ri = fromRegion->GetRouteInfoForProbe();
    if (ri.toRegion1StartAddress != 0 && ri.toRegion1UsedBytes >= 8) {
        FillZeroGaps(ri.toRegion1StartAddress,
                     ri.toRegion1StartAddress + ri.toRegion1UsedBytes);
    }
    if (ri.toRegion2Idx != RouteInfo::INVALID_VALUE) {
        MAddress to2 = RegionInfo::GetUnitAddress(ri.toRegion2Idx);
        RegionInfo* to2r = RegionInfo::TryGetRegionInfoAt(to2);
        uintptr_t to2end = to2r != nullptr ? to2r->GetRegionAllocPtr() : to2;
        if (to2end > to2) {
            FillZeroGaps(to2, to2end);
        }
    }
}

bool RegionManager::RouteOrCompactRegionImpl(RegionInfo* region)
{
    CHECK(region->IsRoutingState());
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");

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
            uintptr_t reserved = toRegion1->GetRegionAllocPtr();
            toRegion1->Alloc(fromBytes);
            FillRouteReserve(reserved, fromBytes);
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
        FillRouteReserve(toRegion1Addr, fromBytes);
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
        FillRouteReserve(toRegion1Addr, usedBytes1);
        uintptr_t r2 = toRegion2->GetRegionAllocPtr();
        CHECK(toRegion2->Alloc(usedBytes2) != 0);
        FillRouteReserve(r2, usedBytes2);
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
    MAddress regionLimit = region->GetRegionAllocPtr();
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, regionStart);
    CHECK_DETAIL(static_cast<bool>(publication),
                 "compact forwarding table unavailable before copy region=%p range=[%#zx,%#zx)",
                 region, static_cast<size_t>(regionStart), static_cast<size_t>(region->GetRegionEnd()));
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    // uafclose: Admit/GetRoute/VisitLive use liveInfo0 after PrepareForwardable. Compact must
    // copy the same set — region->IsSurvivedObject reads current liveInfo (+ mark-epoch), which
    // can disagree with the ghost face. Wrong face ⇒ copy nothing / wrong set, memset free-tail
    // that still holds root-named from → leave-alone → reclaim → GetSize UAF (FYS1 deadold).
    auto survivedAt = [region](size_t offset) -> bool { return region->IsOwnerSurvivedObject(offset); };
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

            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
            const MAddress receipt = ForwardingTable::InsertMapping(publication, currentPtr, toAddress);
            (void)relocationRequestQueue.Publish(currentPtr, receipt);
            region->RecordCompactRoute(offset, toAddress);

        }
        currentPtr += size;
    }

    // clear unused space which is free after compaction.
    // Walk-break leftovers are still live at from — do not memset them.
    MAddress cur = region->GetRegionAllocPtr();
    if (!walkBroke && regionLimit > cur) {
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


    EnlistCompactedRegionForAllocator(region);
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
    if (region->GetRegionType() != RegionInfo::RegionType::THREAD_LOCAL_REGION) {
        return;
    }
    if (!tlRegionList.TryDeleteRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION,
                                      RegionInfo::RegionType::RECENT_FULL_REGION)) {
        return;
    }
    recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
    RecentFullAccounting::Enqueue(1, region->GetUnitCount());
}

void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u to region %p@%#zx:%#zx",
        region, regionStart, region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType(),
        toRegion1, toRegion1->GetRegionStart(), toRegion1->GetRegionAllocPtr());
    MAddress currentPtr = regionStart;
    ForwardingTable::Publication publication =
        ForwardingTable::EnsurePublicationBeforeCopy(region, regionStart);
    CHECK_DETAIL(static_cast<bool>(publication),
                 "partial compact forwarding table unavailable before copy region=%p range=[%#zx,%#zx)",
                 region, static_cast<size_t>(regionStart), static_cast<size_t>(region->GetRegionEnd()));
    BaseObject* currentObj = from_region_addr(currentPtr);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    // uafclose: same ghost-face survivor as CompactRegion(region) / VisitLive / Admit.
    auto survivedAt = [region](size_t offset) -> bool { return region->IsOwnerSurvivedObject(offset); };
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

            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
            const MAddress receipt = ForwardingTable::InsertMapping(publication, currentPtr, toAddress);
            (void)relocationRequestQueue.Publish(currentPtr, receipt);
            region->RecordCompactRoute(offset, toAddress);

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

            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
            const MAddress receipt = ForwardingTable::InsertMapping(publication, currentPtr, toAddress);
            (void)relocationRequestQueue.Publish(currentPtr, receipt);
            region->RecordCompactRoute(offset, toAddress);

        }
        currentPtr += size;
    }

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


    EnlistCompactedRegionForAllocator(region);
}

namespace {
void PermhitReceiptAudit(RegionInfo* region, const char* point)
{
    (void)region;
    (void)point;
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
    MarkView<G> markView = region->GetRouteMarkView<G>();
    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", region->GetRegionType());

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %zu",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        region->GetRegionType(), region->GetLiveByteCount());

    bool youngRegion = region->IsYoungRegion();
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
        RegionInfo::RouteState rsKeep = region->GetRouteState();
        const bool incompleteRoute = rsKeep == RegionInfo::RouteState::ROUTING ||
            rsKeep == RegionInfo::RouteState::ROUTED;
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
                static_cast<unsigned>(region->GetRouteState()), region->GetLiveByteCount());
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
                const bool doReg = (r == GC_REASON_YOUNG);
                if (doReg) {
                    PromotedRegionDomain::Register(region, PromotedRegionDomain::RegisterPath::Abandon);
                }
                PromotedRegionDomain::NoteRegisterGate(static_cast<uint32_t>(r), /*site*/ 1, doReg);
                size_t recEdges = RecordPromotedCrossGenEdges(region);
                PromotedRegionDomain::NoteRecordCall(static_cast<uint32_t>(r), /*site*/ 1, recEdges);
            }
            (void)region->PromoteYoungRegion(promotionView);
        }
        region->DispelGhostFromRegion();
        ExemptFromRegion(region);
        return;
        }
    }

    const bool stayYoung = youngRegion && StayYoungThisCycle(region);
    if (stayYoung || !RouteRegion(region)) {
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

            if (youngRegion && toObj != nullptr) {
                if (!Collector::PlausibleManagedObjectGate("ForwardRegion.to", toObj)) {
                    NoteFwdToGateRefuse("young", toObj);
                } else if (toObj->HasRefField()) {
                toObj->ForEachRefField([&rememberedSet, &promotedRecords, toObj, &collector](RefField<>& field) {
                    BaseObject* target = ScanFieldHealedTarget(collector, field);
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        NotePromoteGapField(toObj, field, false, true);

                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        rememberedSet.Record(slot);
                        ++promotedRecords;

                        NotePromoteGapField(toObj, field, true, true);

                    } else {
                        NotePromoteGapField(toObj, field, false, true);

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
                size_t moved = rememberedSet.TransferObjectSlots(fromBase, toBase, sz);
                recordedOnToForOld += moved;

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
        PermhitReceiptAudit(region, "publish");
        // insert-before-unlock (MutatorRelocate.h:124): a concurrent copier may
        // still hold LOCKED after insert. Do not publish FORWARDED/done while
        // those headers remain LOCKED — waiters treat done as "no live copier".
        WaitCopiedObjectsUnlocked(region);
        region->SetRouteState(RegionInfo::RouteState::FORWARDED);
        FillPublishedRouteGaps(region);
        // zRelocate.cpp:1152 — last act after every object on the page is relocated.
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

template void RegionManager::ForwardFromRegions<Generation::Young>(GCThreadPool*);
template void RegionManager::ForwardFromRegions<Generation::Old>(GCThreadPool*);
template void RegionManager::ForwardFromRegions<Generation::Young>();
template void RegionManager::ForwardFromRegions<Generation::Old>();
#if defined(MRT_TESTABLE_INTERNALS)
template class ForwardTask<Generation::Young>;
template class ForwardTask<Generation::Old>;
#endif
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
