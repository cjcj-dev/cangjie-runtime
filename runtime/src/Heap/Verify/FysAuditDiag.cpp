// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/FysAuditDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace FysAuditDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_FYS_AUDIT")) {
            return true;
        }
        return DiagGate::TokenOn("fysaudit");
    }();
    return on;
}

// Defect class codes (fysaudit §3.1). Keep stable for log parsers.
enum ClassCode : uint8_t {
    CLS_D1 = 1,
    CLS_D2 = 2,
    CLS_D3 = 3,
    CLS_D4 = 4,
    CLS_UNCLASSIFIED = 0,
};

const char* ClassName(uint8_t c)
{
    switch (c) {
        case CLS_D1:
            return "D1";
        case CLS_D2:
            return "D2";
        case CLS_D3:
            return "D3";
        case CLS_D4:
            return "D4";
        default:
            return "unclassified";
    }
}

// Dedup key preserves classification: (slot>>3) xor (class<<56) xor (hType<<48).
// Caps sample spam without collapsing distinct defect classes on the same slot.
uint64_t DedupKey(MAddress slot, uint8_t cls, unsigned holderType)
{
    return (static_cast<uint64_t>(slot) >> 3) ^ (static_cast<uint64_t>(cls) << 56) ^
           (static_cast<uint64_t>(holderType & 0xffu) << 48);
}

bool RetainedWouldDrop(BaseObject* holder, RegionInfo* holderRegion)
{
    if (holder == nullptr || holderRegion == nullptr) {
        return false;
    }
    RegionInfo::RetainedLiveInfoState retainedState = holderRegion->GetRetainedLiveInfoState();
    if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
        return false;
    }
    if (!holderRegion->IsRetainedSnapshotValid()) {
        return false;
    }
    MAddress coveredUpTo = holderRegion->GetRetainedLiveInfoCoveredUpTo();
    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
    if (holderAddress >= coveredUpTo) {
        return false;
    }
    if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
        return true;
    }
    if (holderRegion->IsLargeRegion()) {
        LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
        bool keep = retainedLiveInfo != nullptr ? retainedLiveInfo->IsSurvivedObject(0)
                                                : holderRegion->IsSurvivedObject(0);
        return !keep;
    }
    LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
    if (retainedLiveInfo == nullptr) {
        return false;
    }
    size_t holderOffset = holderRegion->GetAddressOffset(holderAddress);
    return !retainedLiveInfo->IsSurvivedObject(holderOffset);
}

bool IsWeakReferentSlot(BaseObject* holder, MAddress slot)
{
    if (holder == nullptr || !holder->IsWeakRef()) {
        return false;
    }
    MAddress referentSlot = reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE;
    return slot == referentSlot;
}

struct Counters {
    size_t minorRun = 0;
    size_t holdersScanned = 0;
    size_t edgesO2Y = 0;
    size_t inRemset = 0;
    size_t miss = 0;
    size_t d1 = 0;
    size_t d2 = 0;
    size_t d3 = 0;
    size_t d4 = 0;
    size_t unclassified = 0;
    size_t holderMarked = 0;
    size_t holderUnmarked = 0;
    size_t samplesEmitted = 0;
    size_t dedupHits = 0;
    size_t costNs = 0;
    // d1producer: split D1 by whether the write barrier ever recorded this slot at all.
    size_t d1NeverRecorded = 0;   // write-side miss: producer never handed the slot over
    size_t d1RecordedThenLost = 0; // retention/consume-side: recorded earlier, destructive drain removed it
};

Counters g_c;
std::unordered_set<uint64_t> g_dedup;
// d1producer: D1 edges of the current minor, replayed against the remset after the
// conservative pinned/old walk. Bounded so a pathological minor cannot grow it without limit.
std::vector<std::pair<MAddress, BaseObject*>> g_d1Edges;
std::atomic<uint64_t> g_procMiss{ 0 };
std::atomic<uint64_t> g_procD1{ 0 };
std::atomic<uint64_t> g_procD2{ 0 };
std::atomic<uint64_t> g_procD3{ 0 };
std::atomic<uint64_t> g_procD4{ 0 };
std::atomic<uint64_t> g_procUnc{ 0 };
std::atomic<uint64_t> g_procMinors{ 0 };
std::atomic<uint64_t> g_procD1Recovered{ 0 };
std::atomic<uint64_t> g_procD1Residual{ 0 };
std::atomic<uint64_t> g_procD1Truncated{ 0 };
std::atomic<uint64_t> g_procPostPinnedRuns{ 0 };
std::atomic<uint64_t> g_procD1NeverRecorded{ 0 };
std::atomic<uint64_t> g_procD1RecordedThenLost{ 0 };

void EmitSample(uint8_t cls, MAddress slot, BaseObject* holder, RegionInfo* holderRegion, BaseObject* target,
                RegionInfo* targetRegion, bool inRemset, bool holderMarked)
{
    const size_t sampleCap = EnvSizeT("MRT_GCV2_FYS_AUDIT_SAMPLES", 32);
    if (g_c.samplesEmitted >= sampleCap) {
        return;
    }
    unsigned hType = holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->GetRegionType());
    uint64_t key = DedupKey(slot, cls, hType);
    if (!g_dedup.insert(key).second) {
        ++g_c.dedupHits;
        return;
    }
    const size_t dedupCap = EnvSizeT("MRT_GCV2_FYS_AUDIT_DEDUP", 4096);
    if (g_dedup.size() > dedupCap) {
        // Keep classifying; stop expanding dedup + samples.
        return;
    }
    ++g_c.samplesEmitted;
    TypeInfo* hTi = holder != nullptr && holder->IsValidObject() ? holder->GetTypeInfo() : nullptr;
    TypeInfo* tTi = target != nullptr && target->IsValidObject() ? target->GetTypeInfo() : nullptr;
    VLOG(REPORT,
         "[GCV2][fysaudit][EDGE] class=%s slot=%#zx holder=%p hType=%u hYoung=%u hMarked=%u "
         "hRegion=%p hName=%s target=%p tType=%u tYoung=%u tRegion=%p tName=%s inRemset=%u",
         ClassName(cls), static_cast<size_t>(slot), holder, hType,
         holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsYoungRegion()),
         static_cast<unsigned>(holderMarked), holderRegion,
         hTi == nullptr || hTi->GetName() == nullptr ? "?" : hTi->GetName(), target,
         targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->GetRegionType()),
         targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsYoungRegion()), targetRegion,
         tTi == nullptr || tTi->GetName() == nullptr ? "?" : tTi->GetName(), static_cast<unsigned>(inRemset));
}

void CountClass(uint8_t cls)
{
    switch (cls) {
        case CLS_D1:
            ++g_c.d1;
            break;
        case CLS_D2:
            ++g_c.d2;
            break;
        case CLS_D3:
            ++g_c.d3;
            break;
        case CLS_D4:
            ++g_c.d4;
            break;
        default:
            ++g_c.unclassified;
            break;
    }
}

// Classify one O→Y edge at pre-pinned time (mutator remset only).
uint8_t ClassifyPrePinned(BaseObject* holder, MAddress slot, bool inRemset, bool retainedDrop)
{
    const bool weakRef = IsWeakReferentSlot(holder, slot);
    if (weakRef) {
        // D3: weak referent edge — FYS0 may treat recorded weak as strong / miss weak bookkeeping.
        return CLS_D3;
    }
    if (!inRemset) {
        // D1: producer never recorded into mutator remset (compiler Idle bare-store / native).
        return CLS_D1;
    }
    if (retainedDrop) {
        // D2: recorded, but FYS0 retained-holder oracle would drop this holder.
        return CLS_D2;
    }
    // In remset, not weak, retained keeps — not a miss; caller should not count as miss.
    return CLS_UNCLASSIFIED;
}

} // namespace

bool Enabled() { return GateOn(); }

bool ForceProductFullYoungScanFalse() { return GateOn(); }

void OnMinorBegin(size_t minorRunIndex)
{
    if (!GateOn()) {
        return;
    }
    g_c = Counters{};
    g_c.minorRun = minorRunIndex;
    g_dedup.clear();
    g_d1Edges.clear();
}

void CensusPrePinned(size_t minorRunIndex)
{
    if (!GateOn()) {
        return;
    }
    g_c.minorRun = minorRunIndex;
    uint64_t t0 = TimeUtil::NanoSeconds();
    std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
    // d1producer positive control for the sticky bitmap: slots that ARE in the remset right now.
    // If the ever bit never gets set, "D1 is 100% neverRecorded" would be an artefact of a dead
    // instrument rather than a finding, so count how many live remset slots carry the bit.
    size_t snapEver = 0;
    if (Heap::GetHeap().GetRememberedSet().EverRecordedEnabled()) {
        for (MAddress slot : remsetSnap) {
            if (Heap::GetHeap().GetRememberedSet().WasEverRecorded(slot)) {
                ++snapEver;
            }
        }
        VLOG(REPORT, "[GCV2][fysaudit][EVER_CTRL] minor=%zu remsetNow=%zu everSet=%zu (positive control: "
                     "expect >0 whenever the write barrier recorded anything)",
             minorRunIndex, remsetSnap.size(), snapEver);
    }

    Heap::GetHeap().ForEachObj(
        [&remsetSnap](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsYoungRegion() || holderRegion->IsGarbageRegion() ||
                holderRegion->IsFreeRegion()) {
                return;
            }
            ++g_c.holdersScanned;
            const bool holderMarked = holderRegion->IsMarkedObject(holder);
            if (holderMarked) {
                ++g_c.holderMarked;
            } else {
                ++g_c.holderUnmarked;
            }
            const bool retainedDrop = RetainedWouldDrop(holder, holderRegion);
            holder->ForEachRefField(
                [&remsetSnap, holder, holderRegion, holderMarked, retainedDrop](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion == nullptr || !targetRegion->IsYoungRegion()) {
                        return;
                    }
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    ++g_c.edgesO2Y;
                    const bool inRemset = remsetSnap.count(slot) != 0;
                    if (inRemset) {
                        ++g_c.inRemset;
                    }
                    // Interest surface: miss, weak referent, or retained-would-drop recorded edge.
                    const bool weakRef = IsWeakReferentSlot(holder, slot);
                    if (inRemset && !weakRef && !retainedDrop) {
                        return;
                    }
                    ++g_c.miss;
                    uint8_t cls = ClassifyPrePinned(holder, slot, inRemset, retainedDrop);
                    if (cls == CLS_UNCLASSIFIED && !inRemset) {
                        cls = CLS_D1;
                    }
                    if (cls == CLS_UNCLASSIFIED) {
                        // Recorded + weak already handled; recorded+retainedDrop → D2.
                        // Leftover interest edges stay unclassified with raw fields.
                    }
                    CountClass(cls);
                    if (cls == CLS_D1) {
                        // d1producer: exclude the read/retention side. A slot the mutator barrier
                        // never recorded is a write-side miss; one it did record and the
                        // destructive DrainForMinor later dropped belongs to the S1 face.
                        if (Heap::GetHeap().GetRememberedSet().WasEverRecorded(slot)) {
                            ++g_c.d1RecordedThenLost;
                        } else {
                            ++g_c.d1NeverRecorded;
                        }
                    }
                    if (cls == CLS_D1) {
                        // d1producer: keep the slot so CensusPostPinned can ask whether the
                        // conservative pinned/old walk recovered it before DrainForMinor.
                        const size_t keepCap = EnvSizeT("MRT_GCV2_FYS_AUDIT_D1_KEEP", 1u << 20);
                        if (g_d1Edges.size() < keepCap) {
                            g_d1Edges.emplace_back(slot, holder);
                        } else {
                            g_procD1Truncated.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    EmitSample(cls, slot, holder, holderRegion, target, targetRegion, inRemset, holderMarked);
                });
        },
        false);

    g_c.costNs = TimeUtil::NanoSeconds() - t0;
    g_procMiss.fetch_add(g_c.miss, std::memory_order_relaxed);
    g_procD1.fetch_add(g_c.d1, std::memory_order_relaxed);
    g_procD2.fetch_add(g_c.d2, std::memory_order_relaxed);
    g_procD3.fetch_add(g_c.d3, std::memory_order_relaxed);
    g_procD4.fetch_add(g_c.d4, std::memory_order_relaxed);
    g_procUnc.fetch_add(g_c.unclassified, std::memory_order_relaxed);
    g_procMinors.fetch_add(1, std::memory_order_relaxed);
    g_procD1NeverRecorded.fetch_add(g_c.d1NeverRecorded, std::memory_order_relaxed);
    g_procD1RecordedThenLost.fetch_add(g_c.d1RecordedThenLost, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][fysaudit][D1_SIDE] minor=%zu D1=%zu neverRecorded=%zu recordedThenLost=%zu everBitmap=%u",
         g_c.minorRun, g_c.d1, g_c.d1NeverRecorded, g_c.d1RecordedThenLost,
         static_cast<unsigned>(Heap::GetHeap().GetRememberedSet().EverRecordedEnabled()));
    Report("pre-pinned");
}

void CensusPostPinned(size_t minorRunIndex, size_t pinnedRecorded)
{
    if (!GateOn()) {
        return;
    }
    uint64_t t0 = TimeUtil::NanoSeconds();
    // Same buffer CensusPrePinned snapshotted: RecordPinnedCrossGenEdges records into the
    // active buffer and DrainForMinor has not run yet, so this is pre-drain ∪ pinned-walk.
    std::unordered_set<MAddress> snap = Heap::GetHeap().GetRememberedSet().Snapshot();
    size_t recovered = 0;
    size_t residual = 0;
    size_t residualSamples = 0;
    const size_t sampleCap = EnvSizeT("MRT_GCV2_FYS_AUDIT_SAMPLES", 32);
    for (const auto& edge : g_d1Edges) {
        if (snap.count(edge.first) != 0) {
            ++recovered;
            continue;
        }
        ++residual;
        if (residualSamples >= sampleCap) {
            continue;
        }
        ++residualSamples;
        BaseObject* holder = edge.second;
        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
        TypeInfo* hTi = holder != nullptr && holder->IsValidObject() ? holder->GetTypeInfo() : nullptr;
        VLOG(REPORT,
             "[GCV2][fysaudit][EDGE] class=D1R slot=%#zx holder=%p hType=%u hYoung=%u hMarked=%u "
             "hRegion=%p hName=%s inRemset=0 note=pinned_walk_did_not_recover",
             static_cast<size_t>(edge.first), holder,
             holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->GetRegionType()),
             holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsYoungRegion()),
             holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsMarkedObject(holder)),
             holderRegion, hTi == nullptr || hTi->GetName() == nullptr ? "?" : hTi->GetName());
    }
    g_procD1Recovered.fetch_add(recovered, std::memory_order_relaxed);
    g_procD1Residual.fetch_add(residual, std::memory_order_relaxed);
    g_procPostPinnedRuns.fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][fysaudit][post-pinned] minor=%zu d1=%zu recovered=%zu residual=%zu "
         "pinnedRecorded=%zu remsetNow=%zu costNs=%zu",
         minorRunIndex, g_d1Edges.size(), recovered, residual, pinnedRecorded, snap.size(),
         static_cast<size_t>(TimeUtil::NanoSeconds() - t0));
}

// d4ledger: mirror RescanRememberedSet drop reasons for live\consumed (heap only).
// Order matches WCollector.cpp Rescan scrub (region-dead → retained → resolve → bad_target).
// Does not call FindLatestVersion / IsValidObject predicates beyond the same soft checks Rescan uses.
enum class D4DropReason : uint8_t {
    FREE_GARBAGE_HOLDER = 0,
    RETAINED_DEAD = 1,
    STALE_TARGET = 2, // null / non-heap after soft resolve
    BAD_TARGET = 3,   // heap addr but !IsValidObject (Rescan :3667)
    OTHER = 4,        // unexplained — only this means "should have been consumed"
};

const char* D4DropName(D4DropReason r)
{
    switch (r) {
        case D4DropReason::FREE_GARBAGE_HOLDER:
            return "free_garbage_holder";
        case D4DropReason::RETAINED_DEAD:
            return "retained_dead";
        case D4DropReason::STALE_TARGET:
            return "stale_target";
        case D4DropReason::BAD_TARGET:
            return "bad_target";
        default:
            return "other_should_consume";
    }
}

D4DropReason ClassifyLiveNotConsumed(MAddress slot)
{
    RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(slot);
    if (holderRegion == nullptr || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion()) {
        return D4DropReason::FREE_GARBAGE_HOLDER;
    }
    // Soft field read (same shape as Rescan pre-resolve peek; no FindLatestVersion).
    HeapSlot<>* field = &HeapSlotAt<>(slot);
    RefField<> peek(*field);
    BaseObject* target = to_object(peek.GetTargetObject());
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return D4DropReason::STALE_TARGET;
    }
    if (!target->IsValidObject()) {
        return D4DropReason::BAD_TARGET;
    }
    // Target looks valid at post-rescan time — may have been retained-dropped (needs origin)
    // or race-resolved after Rescan; residual for should-consume analysis.
    return D4DropReason::OTHER;
}

void PostRescan(const std::unordered_set<MAddress>& rememberedSlots,
                const std::unordered_set<MAddress>& liveRememberedSlots,
                const std::unordered_set<MAddress>& consumedSlots, const std::unordered_set<MAddress>& weakSlots)
{
    if (!GateOn()) {
        return;
    }
    size_t d4Local = 0;
    size_t d2Local = 0;
    size_t d4Free = 0;
    size_t d4Retained = 0;
    size_t d4Stale = 0;
    size_t d4Bad = 0;
    size_t d4Other = 0;
    size_t d4NonHeap = 0;
    const size_t sampleCap = EnvSizeT("MRT_GCV2_FYS_AUDIT_SAMPLES", 32);
    for (MAddress slot : liveRememberedSlots) {
        if (consumedSlots.count(slot) != 0) {
            continue;
        }
        if (weakSlots.count(slot) != 0) {
            continue;
        }
        // External/static remset slots are live-not-consumed by design (Rescan skippedNotHeap).
        // Never call TryGetRegionInfoAt on non-heap — GetUnitIdxAt OOB aborts.
        if (!Heap::IsHeapAddress(slot)) {
            ++d4NonHeap;
            continue;
        }
        // live-but-not-consumed under FYS0 product path → D4 ledger split.
        ++d4Local;
        ++g_c.d4;
        ++g_c.miss;
        D4DropReason reason = ClassifyLiveNotConsumed(slot);
        switch (reason) {
            case D4DropReason::FREE_GARBAGE_HOLDER:
                ++d4Free;
                break;
            case D4DropReason::RETAINED_DEAD:
                ++d4Retained;
                break;
            case D4DropReason::STALE_TARGET:
                ++d4Stale;
                break;
            case D4DropReason::BAD_TARGET:
                ++d4Bad;
                break;
            default:
                ++d4Other;
                break;
        }
        if (g_c.samplesEmitted < sampleCap) {
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
            unsigned hType = region == nullptr ? 0u : static_cast<unsigned>(region->GetRegionType());
            uint64_t key = DedupKey(slot, CLS_D4, hType);
            if (g_dedup.insert(key).second) {
                ++g_c.samplesEmitted;
                HeapSlot<>* field = &HeapSlotAt<>(slot);
                RefField<> peek(*field);
                BaseObject* target = to_object(peek.GetTargetObject());
                RegionInfo* tRegion =
                    target != nullptr && Heap::IsHeapAddress(target)
                        ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target))
                        : nullptr;
                VLOG(REPORT,
                     "[GCV2][fysaudit][EDGE] class=D4 slot=%#zx holder=%p hType=%u hYoung=0 hMarked=0 "
                     "hRegion=%p hName=? target=%p tType=%u tYoung=%u tRegion=%p tName=? inRemset=1 "
                     "note=live_not_consumed drop=%s",
                     static_cast<size_t>(slot), nullptr, hType, region, target,
                     tRegion == nullptr ? 0u : static_cast<unsigned>(tRegion->GetRegionType()),
                     tRegion == nullptr ? 0u : static_cast<unsigned>(tRegion->IsYoungRegion()), tRegion,
                     D4DropName(reason));
            }
        }
    }
    // D2 post-check: recorded slots dropped from live by retained filter (not in live, not weak).
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        if (weakSlots.count(slot) != 0) {
            continue;
        }
        if (liveRememberedSlots.count(slot) != 0) {
            continue;
        }
        // Slot was recorded but not admitted to live — under FYS0 this is retained/weak path.
        // weak already skipped; remainder is retained-holder drop candidate → D2.
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        ++d2Local;
        ++g_c.d2;
        ++g_c.miss;
        if (g_c.samplesEmitted < sampleCap) {
            unsigned hType = static_cast<unsigned>(region->GetRegionType());
            uint64_t key = DedupKey(slot, CLS_D2, hType);
            if (g_dedup.insert(key).second) {
                ++g_c.samplesEmitted;
                VLOG(REPORT,
                     "[GCV2][fysaudit][EDGE] class=D2 slot=%#zx holder=%p hType=%u hYoung=%u hMarked=0 "
                     "hRegion=%p hName=? target=%p tType=0 tYoung=0 tRegion=%p tName=? inRemset=1 "
                     "note=recorded_not_live_retained_drop",
                     static_cast<size_t>(slot), nullptr, hType,
                     static_cast<unsigned>(region->IsYoungRegion()), region, nullptr, nullptr);
            }
        }
    }
    g_procD2.fetch_add(d2Local, std::memory_order_relaxed);
    g_procD4.fetch_add(d4Local, std::memory_order_relaxed);
    g_procMiss.fetch_add(d2Local + d4Local, std::memory_order_relaxed);
    VLOG(REPORT,
         "[GCV2][fysaudit][post-rescan] minor=%zu remembered=%zu live=%zu consumed=%zu "
         "D2_retainedDrop=%zu D4_liveNotConsumed=%zu "
         "D4_freeHolder=%zu D4_retainedDead=%zu D4_staleTarget=%zu D4_badTarget=%zu "
         "D4_otherShouldConsume=%zu D4_nonHeapSkipped=%zu",
         g_c.minorRun, rememberedSlots.size(), liveRememberedSlots.size(), consumedSlots.size(), d2Local, d4Local,
         d4Free, d4Retained, d4Stale, d4Bad, d4Other, d4NonHeap);
    (void)rememberedSlots;
}

void Report(const char* tag)
{
    if (!GateOn()) {
        return;
    }
    const char* t = tag != nullptr ? tag : "census";
    VLOG(REPORT,
         "[GCV2][fysaudit][%s] minor=%zu holders=%zu OY=%zu inRemset=%zu miss=%zu "
         "D1=%zu D2=%zu D3=%zu D4=%zu unclassified=%zu hMarked=%zu hUnmarked=%zu "
         "samples=%zu dedupHits=%zu costNs=%zu productFYS=0 auditWalk=full_non_young "
         "reachableSlotsFilter=0",
         t, g_c.minorRun, g_c.holdersScanned, g_c.edgesO2Y, g_c.inRemset, g_c.miss, g_c.d1, g_c.d2, g_c.d3, g_c.d4,
         g_c.unclassified, g_c.holderMarked, g_c.holderUnmarked, g_c.samplesEmitted, g_c.dedupHits, g_c.costNs);
}

void DumpProcessTotals(const char* tag)
{
    if (!GateOn()) {
        return;
    }
    const char* t = tag != nullptr ? tag : "totals";
    VLOG(REPORT,
         "[GCV2][fysaudit][TOTAL] tag=%s minors=%llu miss=%llu D1=%llu D2=%llu D3=%llu D4=%llu "
         "unclassified=%llu (pre-pinned D1≈producer; D2/D4 also accumulate post-rescan)",
         t, static_cast<unsigned long long>(g_procMinors.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procMiss.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD2.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD3.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD4.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procUnc.load(std::memory_order_relaxed)));
    // d1producer: D1 is measured against the mutator remset; the product FYS=0 path also has the
    // always-on pinned/old walk before DrainForMinor. residual is what FYS=0 actually loses.
    VLOG(REPORT,
         "[GCV2][fysaudit][POSTPIN_TOTAL] tag=%s postPinnedRuns=%llu d1Recovered=%llu d1Residual=%llu "
         "d1Truncated=%llu d1NeverRecorded=%llu d1RecordedThenLost=%llu everBitmap=%u",
         t, static_cast<unsigned long long>(g_procPostPinnedRuns.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1Recovered.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1Residual.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1Truncated.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1NeverRecorded.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_procD1RecordedThenLost.load(std::memory_order_relaxed)),
         static_cast<unsigned>(Heap::GetHeap().GetRememberedSet().EverRecordedEnabled()));
}

} // namespace FysAuditDiag
} // namespace MapleRuntime
