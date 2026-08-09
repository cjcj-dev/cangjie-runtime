// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <array>
#include <atomic>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <csignal>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <unistd.h>
#endif

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Base/SysCall.h"
#endif
#include "Concurrency/Concurrency.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/DiffPathExplainer.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Heap/Verify/IdleEdgeDiag.h"
#include "Heap/Verify/PlainCensus.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Verify/VerifyRegions.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "securec.h"
#endif

namespace MapleRuntime {
static_assert(sizeof(RefField<false>) == 8, "RefField colour layout must preserve the 64-bit ABI");

// Phase A (ops/design/G1_WRITE_BARRIER_DESIGN.md §3.6): races in ForwardUpdateRawRef.
// Total counts every lost CAS -- it is the positive control, proving the site is reached at all,
// so a zero in StillBad means "did not happen" rather than "never wired up". StillBad counts the
// races whose winning value still needs the barrier: zero under today's two-state encoding, and
// a legal (not defective) state once phase C gives good a non-zero colour.
// Report with MRT_GCV2_FORWARD_RACE_ACCOUNT=1.
std::atomic<size_t> g_forwardRaceTotalCount{ 0 };
std::atomic<size_t> g_forwardRaceStillBadCount{ 0 };

// installdomain: positive control — how often Resolve/Fix would install a ghost-from that is
// outside GetRoute's liveInfo0 survivor domain. Grant paints that bit before route geometry.
// Report with MRT_GCV2_INSTALLDOMAIN_ACCOUNT=1 (also always VLOG once per minor if >0).
std::atomic<size_t> g_installDomainGrant{ 0 };
std::atomic<size_t> g_installDomainAlready{ 0 };
std::atomic<size_t> g_installDomainTooLate{ 0 };
std::atomic<size_t> g_installDomainSkip{ 0 };

// nullslot: count product paths that CAS-install nullptr into a ref field.
// MRT_GCV2_NULLSLOT=1 → LOG each write (cap 64/path) + totals; default off.
namespace {
bool NullslotProbeEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_NULLSLOT");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

std::atomic<size_t> g_nullslotF3{ 0 };
std::atomic<size_t> g_nullslotResolve{ 0 };
std::atomic<size_t> g_nullslotRemset{ 0 };

void NoteNullslotWrite(const char* path, BaseObject* holder, void* field, BaseObject* from, BaseObject* latest,
                       std::atomic<size_t>* pathCount)
{
    size_t n = pathCount->fetch_add(1, std::memory_order_relaxed);
    if (!NullslotProbeEnabled() || n >= 64) {
        return;
    }
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    LOG(RTLOG_ERROR,
        "[GCV2][nullslot] path=%s n=%zu holder=%p field=%p from=%p latest=%p phase=%s(%u) "
        "holderValid=%d fromHeap=%u latestHeap=%u",
        path, n, holder, field, from, latest, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
        holder != nullptr && Heap::IsHeapAddress(holder) ? static_cast<int>(holder->IsValidObject()) : -1,
        static_cast<unsigned>(from != nullptr && Heap::IsHeapAddress(from)),
        static_cast<unsigned>(latest != nullptr && Heap::IsHeapAddress(latest)));
}
} // namespace

void ReportForwardRaceCounts()
{
    static const bool account = []() {
        const char* v = std::getenv("MRT_GCV2_FORWARD_RACE_ACCOUNT");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    if (!account) {
        return;
    }
    VLOG(REPORT, "[GCV2][fwdrace] total=%zu still_bad=%zu",
         g_forwardRaceTotalCount.load(std::memory_order_relaxed),
         g_forwardRaceStillBadCount.load(std::memory_order_relaxed));
}

namespace {
// T1 ledger-cost probe (setbitmap O1③): default off.
// MRT_GCV2_LEDGER_COST=1 → time+count every insert/lookup on objects/slots/weaks
// MRT_GCV2_LEDGER_COST=2 → count only (no NanoSeconds; for overhead control)
// Report line: [GCV2][ledger-cost] ...
struct MinorLedgerCost {
    uint64_t objInsNs = 0;
    uint64_t objInsN = 0;
    uint64_t objInsNew = 0;
    uint64_t objLookNs = 0;
    uint64_t objLookN = 0;
    uint64_t slotInsNs = 0;
    uint64_t slotInsN = 0;
    uint64_t slotInsNew = 0;
    uint64_t slotLookNs = 0;
    uint64_t slotLookN = 0;
    uint64_t weakInsNs = 0;
    uint64_t weakInsN = 0;
    uint64_t weakInsNew = 0;
    uint64_t weakLookNs = 0;
    uint64_t weakLookN = 0;

    static int Mode()
    {
        static const int mode = []() {
            const char* v = std::getenv("MRT_GCV2_LEDGER_COST");
            if (v == nullptr || v[0] == '\0' || std::strcmp(v, "0") == 0) {
                return 0;
            }
            if (std::strcmp(v, "2") == 0 || std::strcmp(v, "count") == 0) {
                return 2;
            }
            return 1;
        }();
        return mode;
    }

    void Reset() { *this = MinorLedgerCost{}; }

    void Report() const
    {
        if (Mode() == 0) {
            return;
        }
        const uint64_t insNs = objInsNs + slotInsNs + weakInsNs;
        const uint64_t lookNs = objLookNs + slotLookNs + weakLookNs;
        VLOG(REPORT,
             "[GCV2][ledger-cost] mode=%d "
             "obj_ins_n=%llu obj_ins_new=%llu obj_ins_ns=%llu "
             "obj_look_n=%llu obj_look_ns=%llu "
             "slot_ins_n=%llu slot_ins_new=%llu slot_ins_ns=%llu "
             "slot_look_n=%llu slot_look_ns=%llu "
             "weak_ins_n=%llu weak_ins_new=%llu weak_ins_ns=%llu "
             "weak_look_n=%llu weak_look_ns=%llu "
             "ins_total_ns=%llu look_total_ns=%llu all_ns=%llu",
             Mode(),
             static_cast<unsigned long long>(objInsN), static_cast<unsigned long long>(objInsNew),
             static_cast<unsigned long long>(objInsNs),
             static_cast<unsigned long long>(objLookN), static_cast<unsigned long long>(objLookNs),
             static_cast<unsigned long long>(slotInsN), static_cast<unsigned long long>(slotInsNew),
             static_cast<unsigned long long>(slotInsNs),
             static_cast<unsigned long long>(slotLookN), static_cast<unsigned long long>(slotLookNs),
             static_cast<unsigned long long>(weakInsN), static_cast<unsigned long long>(weakInsNew),
             static_cast<unsigned long long>(weakInsNs),
             static_cast<unsigned long long>(weakLookN), static_cast<unsigned long long>(weakLookNs),
             static_cast<unsigned long long>(insNs), static_cast<unsigned long long>(lookNs),
             static_cast<unsigned long long>(insNs + lookNs));
    }
};

thread_local MinorLedgerCost g_minorLedgerCost;

// T2 closure-equality probe (setbitmap2): default off.
// MRT_GCV2_CLOSURE_HASH=1 → dump normalized hash of product reachableVec (diagnostic only).
// MRT_GCV2_CLOSURE_HASH=2 → in-process dual claim: product path + independent legacy set walk
//   on the same roots/remset; compare absolute pointer sets (same process ⇒ real equality).
// Catches: any missing/extra object between SETBITMAP claim styles under identical roots.
// Misses (mode1 only): cross-process ASLR/alloc noise; mode2 is same-input.
struct ClosureHashProbe {
    static int Mode()
    {
        static const int mode = []() {
            const char* v = std::getenv("MRT_GCV2_CLOSURE_HASH");
            if (v == nullptr || v[0] == '\0' || std::strcmp(v, "0") == 0) {
                return 0;
            }
            if (std::strcmp(v, "2") == 0) {
                return 2;
            }
            return 1; // "1" or any other truthy
        }();
        return mode;
    }

    static bool Dual() { return Mode() == 2; }
    static bool Dump() { return Mode() >= 1; }

    static uint64_t Fnv1a(uint64_t h, uint64_t x)
    {
        constexpr uint64_t kPrime = 0x100000001b3ULL;
        h ^= x;
        h *= kPrime;
        return h;
    }

    static uint64_t PtrSetHash(const std::vector<BaseObject*>& vec)
    {
        std::vector<uint64_t> addrs;
        addrs.reserve(vec.size());
        for (BaseObject* o : vec) {
            if (o != nullptr) {
                addrs.push_back(reinterpret_cast<uint64_t>(o));
            }
        }
        std::sort(addrs.begin(), addrs.end());
        // unique
        addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint64_t a : addrs) {
            h = Fnv1a(h, a);
        }
        return h;
    }

    static void ReportDump(size_t minorRun, const std::vector<BaseObject*>& reachableVec, bool useBitmap,
                           bool fullYoungScan)
    {
        if (!Dump()) {
            return;
        }
        uint64_t h = PtrSetHash(reachableVec);
        VLOG(REPORT,
             "[GCV2][closure-hash] run=%zu count=%zu ptr_hash=%llx use=%d fullYoung=%d",
             minorRun, reachableVec.size(), static_cast<unsigned long long>(h),
             static_cast<int>(useBitmap), static_cast<int>(fullYoungScan));
    }

    // Compare product reachableVec vs dual legacy set walk result.
    static void ReportEqual(size_t minorRun, const std::vector<BaseObject*>& product,
                            const std::vector<BaseObject*>& dual, bool productUseBitmap, bool fullYoungScan)
    {
        std::vector<uint64_t> a;
        std::vector<uint64_t> b;
        a.reserve(product.size());
        b.reserve(dual.size());
        for (BaseObject* o : product) {
            if (o != nullptr) {
                a.push_back(reinterpret_cast<uint64_t>(o));
            }
        }
        for (BaseObject* o : dual) {
            if (o != nullptr) {
                b.push_back(reinterpret_cast<uint64_t>(o));
            }
        }
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        b.erase(std::unique(b.begin(), b.end()), b.end());
        const bool equal = (a == b);
        size_t onlyA = 0;
        size_t onlyB = 0;
        if (!equal) {
            std::vector<uint64_t> diff;
            std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff));
            onlyA = diff.size();
            diff.clear();
            std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(diff));
            onlyB = diff.size();
        }
        VLOG(REPORT,
             "[GCV2][closure-eq] run=%zu equal=%d prod_n=%zu dual_n=%zu only_prod=%zu only_dual=%zu "
             "prod_use=%d fullYoung=%d",
             minorRun, static_cast<int>(equal), a.size(), b.size(), onlyA, onlyB,
             static_cast<int>(productUseBitmap), static_cast<int>(fullYoungScan));
    }
};

template <typename SetT, typename KeyT>
bool LedgerInsert(SetT& set, const KeyT& key, uint64_t& n, uint64_t& nNew, uint64_t& ns)
{
    const int mode = MinorLedgerCost::Mode();
    if (mode == 0) {
        return set.insert(key).second;
    }
    if (mode == 2) {
        ++n;
        auto r = set.insert(key);
        if (r.second) {
            ++nNew;
        }
        return r.second;
    }
    uint64_t t0 = TimeUtil::NanoSeconds();
    auto r = set.insert(key);
    ns += TimeUtil::NanoSeconds() - t0;
    ++n;
    if (r.second) {
        ++nNew;
    }
    return r.second;
}

template <typename SetT, typename KeyT>
size_t LedgerCount(const SetT& set, const KeyT& key, uint64_t& n, uint64_t& ns)
{
    const int mode = MinorLedgerCost::Mode();
    if (mode == 0) {
        return set.count(key);
    }
    if (mode == 2) {
        ++n;
        return set.count(key);
    }
    uint64_t t0 = TimeUtil::NanoSeconds();
    size_t c = set.count(key);
    ns += TimeUtil::NanoSeconds() - t0;
    ++n;
    return c;
}
} // namespace

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
namespace {
struct UntagRefFieldBreadcrumb {
    const void* holder = nullptr;
    const void* field = nullptr;
    const void* target = nullptr;
    const void* caller = nullptr;
    size_t fieldOffset = 0;
    volatile sig_atomic_t active = 0;
};

thread_local UntagRefFieldBreadcrumb untagRefFieldBreadcrumb;
} // namespace

void PrintUntagRefFieldBreadcrumb() noexcept
{
    if (untagRefFieldBreadcrumb.active == 0) {
        return;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    char buf[320];
    int n = sprintf_s(buf, sizeof(buf),
                      "%d E GC untag breadcrumb: holder=%p field=%p field_offset=%zu target=%p caller_pc=%p\n",
                      static_cast<int>(GetTid()), untagRefFieldBreadcrumb.holder, untagRefFieldBreadcrumb.field,
                      untagRefFieldBreadcrumb.fieldOffset, untagRefFieldBreadcrumb.target,
                      untagRefFieldBreadcrumb.caller);
    if (n > 0) {
        (void)write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}
#endif

bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    // filter const string object.
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {
        regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}

bool WCollector::MarkObject(BaseObject* obj) const
{
    // markfloor: work stack may hold RawArray+8 interiors (tip word = length, e.g. 0x200).
    // Return true ⇒ ConcurrentMarkingWork treats as already-marked and skips HasRefField.
    if (!Collector::PlausibleManagedObjectGate("WCollector::MarkObject", obj)) {
        return true;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    size_t objectSize = obj->GetSize();
    bool marked = region->MarkObject(obj, objectSize);
    if (!marked) {
        region->AddLiveByteCount(objectSize);
        (void)region;
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %zu", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    // getsize7: ResurrectObject → GetSize; finalizer work-stack should be gated but base path was not.
    if (!Collector::PlausibleManagedObjectGate("WCollector::ResurrectObject", obj)) {
        return true;
    }
    bool resurrected = region->ResurrectObject(obj, offset);
        if (!resurrected) {
            region->AddLiveByteCount(obj->GetSize());
            DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %zu", region, region->GetRegionStart(),
                 obj, obj->GetTypeInfo(), obj->GetSize(), region->GetLiveByteCount());
        }
        return resurrected;
}

// this api updates current pointer as well as old pointer, caller should take care of this.
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (oldRef.IsTagged()) {
        fromObj = to_object(oldRef.GetTargetObject());
        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj);
        }
        if (toObj == nullptr) {
            return false;
        }
        // R7：写回必须经规范色单产地，禁 plain RefField<>(toObj)。
        // expected 仍是 observed-raw（oldRef.GetFieldValue()）；模板 = GetAndTryTagRefField。
        RefField<> tmpField = GetAndTryTagRefField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, raw(oldRef.GetFieldValue()),
                     raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, raw(oldRef.GetFieldValue()), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     raw(oldRef.GetFieldValue()), raw(field.GetFieldValue()), raw(tmpField.GetFieldValue()));
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, raw(oldRef.GetFieldValue()),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}
bool WCollector::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef);
}
// this api untags current pointer as well as old pointer, caller should take care of this.
bool WCollector::TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const
{
    for (;;) {
        RefField<> oldRef(field);
        if (!oldRef.IsTagged()) {
            return false;
        }
        target = to_object(oldRef.GetTargetObject());
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
        untagRefFieldBreadcrumb.active = 0;
        untagRefFieldBreadcrumb.holder = obj;
        untagRefFieldBreadcrumb.field = &field;
        untagRefFieldBreadcrumb.target = target;
        untagRefFieldBreadcrumb.caller = __builtin_return_address(0);
        untagRefFieldBreadcrumb.fieldOffset =
            obj == nullptr ? static_cast<size_t>(-1) : BaseObject::FieldOffset(obj, &field);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        untagRefFieldBreadcrumb.active = 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
        const bool isValidTarget = target->IsValidObject();
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
        if (LIKELY(isValidTarget)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            untagRefFieldBreadcrumb.active = 0;
        }
#endif
        if (UNLIKELY(!isValidTarget)) {
            static const bool f3Region = []() {
                const char* value = std::getenv("MRT_GCV2_F3_REGION");
                return value != nullptr && std::strcmp(value, "1") == 0;
            }();
            if (f3Region) {
                const bool targetInHeap = Heap::IsHeapAddress(target);
                const bool holderInHeap = obj != nullptr && Heap::IsHeapAddress(obj);
                const bool targetInGhost = targetInHeap && RegionInfo::InGhostFromRegion(target);
                RegionInfo* targetCurrent = targetInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target))
                    : nullptr;
                RegionInfo* targetGhost = targetInHeap
                    ? RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target))
                    : nullptr;
                RegionInfo* holderCurrent = holderInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj))
                    : nullptr;
                RegionInfo* holderGhost = holderInHeap
                    ? RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj))
                    : nullptr;
                RegionInfo* selectedTargetRegion = targetInGhost ? targetGhost : targetCurrent;
                const auto dumpRegion = [](const char* role, BaseObject* object, RegionInfo* region) {
                    if (region == nullptr) {
                        VLOG(REPORT,
                             "[GCV2][F3_REGION][region] role=%s object=%p region=null "
                             "NOT_AVAILABLE_no_region_metadata",
                             role, object);
                        return;
                    }
                    VLOG(REPORT,
                         "[GCV2][F3_REGION][region] role=%s object=%p region=%p type=%u unmovable=%u "
                         "young=%u youngAge=%u routeState=%u from=%u ghost=%u start=%#zx end=%#zx "
                         "alloc=%#zx snapshotEpoch=%llu liveBytes=%zu",
                         role, object, region, static_cast<unsigned>(region->GetRegionType()),
                         static_cast<unsigned>(region->IsUnmovableFromRegion()),
                         static_cast<unsigned>(region->IsYoungRegion()), static_cast<unsigned>(region->GetYoungAge()),
                         static_cast<unsigned>(region->GetRouteState()), static_cast<unsigned>(region->IsFromRegion()),
                         static_cast<unsigned>(region->IsGhostFromRegion()),
                         static_cast<size_t>(region->GetRegionStart()), static_cast<size_t>(region->GetRegionEnd()),
                         static_cast<size_t>(region->GetRegionAllocPtr()),
                         static_cast<unsigned long long>(region->GetSnapshotEpoch()), region->GetLiveByteCount());
                };

                const GCPhase phase = GetGCPhase();
                const uint64_t gcStartNs = GCStats::GetPrevGCStartTime();
                VLOG(REPORT,
                     "[GCV2][F3_REGION] target=%p field=%p holder=%p targetInHeap=%u holderInHeap=%u "
                     "unit.inGhostFromRegion=%u selectedBranch=%s targetCurrent=%p targetGhost=%p selected=%p "
                     "phase=%s(%u) completedGcCount=%zu gcStartNs=%llu",
                     target, &field, obj, static_cast<unsigned>(targetInHeap), static_cast<unsigned>(holderInHeap),
                     static_cast<unsigned>(targetInGhost), targetInGhost ? "ghost" : "current", targetCurrent,
                     targetGhost, selectedTargetRegion, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
                     g_gcCount, static_cast<unsigned long long>(gcStartNs));
                dumpRegion("target_current_pre_find", target, targetCurrent);
                dumpRegion("target_ghost_pre_find", target, targetGhost);
                dumpRegion("holder_current", obj, holderCurrent);
                dumpRegion("holder_ghost", obj, holderGhost);

                char targetClear[384];
                char holderClear[384];
                const bool targetWasCleared = targetInHeap &&
                    TraceClear::Lookup(reinterpret_cast<MAddress>(target), targetClear, sizeof(targetClear));
                const bool holderWasCleared = holderInHeap &&
                    TraceClear::Lookup(reinterpret_cast<MAddress>(obj), holderClear, sizeof(holderClear));
                if (!targetInHeap) {
                    std::snprintf(targetClear, sizeof(targetClear), "NOT_AVAILABLE_target_not_in_heap");
                }
                if (!holderInHeap) {
                    std::snprintf(holderClear, sizeof(holderClear), "NOT_AVAILABLE_holder_not_in_heap");
                }
                VLOG(REPORT,
                     "[GCV2][F3_REGION][lifecycle] targetClearedRecent=%u targetClear={%s} "
                     "holderClearedRecent=%u holderClear={%s} "
                     "targetRecycledSinceForward=NOT_AVAILABLE_no_forward_start_snapshotEpoch_baseline",
                     static_cast<unsigned>(targetWasCleared), targetClear, static_cast<unsigned>(holderWasCleared),
                     holderClear);

                char ghostReclaim[384] = "NOT_AVAILABLE_target_not_in_heap";
                char dirtyTake[384] = "NOT_AVAILABLE_target_not_in_heap";
                char garbageReuse[384] = "NOT_AVAILABLE_target_not_in_heap";
                char clearGhost[384] = "NOT_AVAILABLE_target_not_in_heap";
                char dispel[384] = "NOT_AVAILABLE_target_not_in_heap";
                const bool targetGhostReclaimed = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "ghost_reclaim", gcStartNs, ghostReclaim,
                    sizeof(ghostReclaim));
                const bool targetDirtyTaken = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "dirty_take", gcStartNs, dirtyTake, sizeof(dirtyTake));
                const bool targetGarbageReused = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "garbage_reuse", gcStartNs, garbageReuse,
                    sizeof(garbageReuse));
                const bool targetGhostCleared = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "clear_ghost", gcStartNs, clearGhost,
                    sizeof(clearGhost));
                const bool targetDispelled = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "dispel", gcStartNs, dispel, sizeof(dispel));
                VLOG(REPORT,
                     "[GCV2][F3_REGION][supply-path] ghostReclaim=%u dirtyTake=%u garbageReuse=%u "
                     "clearGhost=%u dispel=%u pathConfirmedGhostReclaimDirtyReuse=%u "
                     "ghostReclaim={%s} dirtyTake={%s} garbageReuse={%s} clearGhost={%s} dispel={%s}",
                     static_cast<unsigned>(targetGhostReclaimed), static_cast<unsigned>(targetDirtyTaken),
                     static_cast<unsigned>(targetGarbageReused), static_cast<unsigned>(targetGhostCleared),
                     static_cast<unsigned>(targetDispelled),
                     static_cast<unsigned>(targetGhostReclaimed && targetDirtyTaken), ghostReclaim, dirtyTake,
                     garbageReuse, clearGhost, dispel);

                BaseObject* toVersion = targetInHeap ? FindToVersion(target) : nullptr;
                const bool toInHeap = toVersion != nullptr && Heap::IsHeapAddress(toVersion);
                const int toValid = toInHeap ? static_cast<int>(toVersion->IsValidObject()) : -1;
                RegionInfo* toRegion = toInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(toVersion))
                    : nullptr;
                VLOG(REPORT,
                     "[GCV2][F3_REGION][forward] FindToVersion=%p toInHeap=%u toValid=%d "
                     "findMayRouteRegion=%u TryForwardObject=NOT_CALLED_side_effectful_RouteRegion_and_object_copy",
                     toVersion, static_cast<unsigned>(toInHeap), toValid, static_cast<unsigned>(targetInHeap));
                dumpRegion("to_version_after_find", toVersion, toRegion);
                dumpRegion("target_ghost_after_find", target, targetGhost);
            }
        }
        // Anchor main 2f1bc8355e92dbf01c063050b5c9a2947c711d64
        CHECK_DETAIL(isValidTarget, "TryUntagRefField encounters invalid tagged target %p at field %p", target,
                     &field);
        RefField<> newRef(target);
        ScopedPlainWriter tag(PlainWriterSite::TryUntag);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
            }
            return true;
        }
    }

    return false;
}

// RefFieldRoot is root in tagged pointer format.
void WCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // A mark-good root has passed this mark epoch and is necessarily load-good
    // (OpenJDK zAddress.inline.hpp:658-664).
    if (is_mark_good(oldField)) {
        // Anchor main 8cd248497dd8c251ca824d9f089d5e30125c80c9
        BaseObject* target = to_object(oldField.GetTargetObject());
        // Plain/uncoloured non-null is mark-good under g_cjMarkBadMask; mirror the slow path.
        // Reject non-heap: do not call make_load_good (remap would touch non-heap).
        if (!Collector::MarkGoodHeapGate("EnumRefFieldRoot", target)) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("EnumRefFieldRoot", target)) {
            return;
        }
        CHECK_DETAIL(target->IsValidObject(), "Enum static root %p(%p) encounters invalid object", target, &field);
        rootSet.push_back(target);
        return;
    }

    BaseObject* latest = make_load_good(oldField);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("EnumRefFieldRoot.slow", latest)) {
        return;
    }
    if (VerifyRoots::Enabled()) {
        RootVerifyContext vctx;
        vctx.phase = "EnumRefFieldRoot";
        vctx.kind = RootKind::STATIC_ROOT;
        VerifyRoots::VerifyRootPayload(vctx, &field, latest);
    }
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    // static roots stay Phase-C coloured (writable statics need colour; rostatic skips non-heap CAS).
    // plainroots only applies to stack/reg ObjectRef slots (RootSlotWriteback via !IsHeapAddress).
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()),
             raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, raw(oldField.GetFieldValue()), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(latest);
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    zaddress_unsafe observed = ref.LoadPlain();
    if (is_null(observed)) {
        return;
    }

    // RootSlot contains an uncoloured address. Constructing a local HeapSlot is
    // only a bit-layout decoder for legacy coloured roots at external ABI edges;
    // the root storage itself is never exposed as a HeapSlot.
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* root = to_object(observedBits.GetTargetObject());
    if (root == nullptr || !Heap::IsHeapAddress(root)) {
        return;
    }
    if (IsGhostFromObject(root)) {
        BaseObject* to = FindToVersion(root);
        if (to != nullptr) {
            root = to;
        }
    }
    if (!Collector::PlausibleManagedObjectGate("EnumAndTagRawRoot.plain", root)) {
        // introot: a raw-root stack-map entry may still identify RawArray+8.
        // The paired derived path cannot reach this branch because it is a DerivedSlot.
        BaseObject* host = Collector::TryRecoverInteriorBase(root);
        if (host != nullptr && host->IsValidObject()) {
            HealRoot(ref, from_object(root));
            rootSet.push_back(host);
        }
        return;
    }
    if (VerifyRoots::Enabled()) {
        RootVerifyContext vctx;
        vctx.phase = "EnumAndTagRawRoot.plain";
        vctx.kind = RootKind::RUNTIME_ROOT;
        VerifyRoots::VerifyRootPayload(vctx, &ref, root);
    }
    CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
    HealRoot(ref, from_object(root));
    rootSet.push_back(root);
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
{
    RefField<> oldField(field);
    if (is_mark_good(oldField)) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        // zbisect: plain non-heap (0x55–0x65) was admitted here → IsMarkedObject → GetUnitIdxAt OOB.
        // Skip field on reject — same as pre-zcolor7 slow path for plain non-heap.
        if (!Collector::MarkGoodHeapGate("TraceRefField", targetObj)) {
            return;
        }
        // markfloor: skip interiors (RawArray+8 etc.) before IsValidObject/GetSize.
        if (!Collector::PlausibleManagedObjectGate("TraceRefField", targetObj)) {
            return;
        }
        // Anchor main 9a124c4f14ddd5944330ddbf68d1659cbb629e56
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by strong object %p: %s and offset %zd", targetObj, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
        if (!IsMarkedObject(targetObj)) {
            workStack.push_back(targetObj);
        }
        return;
    }

    BaseObject* latest = make_load_good(oldField);

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("TraceRefField.slow", latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by strong object %p: %s and offset %zd",
                 latest, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
             raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    }

    if (!IsMarkedObject(latest)) {
        workStack.push_back(latest);
    }
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto visitor = [this, obj, &workStack](RefField<>& field) { TraceRefField(obj, field, workStack); };
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (!typeInfo->HasRefField()) {
        return;
    }

    if (UNLIKELY(typeInfo->IsRawArray())) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        MIndex arrayLength = array->GetLength();
        TypeInfo* componentTypeInfo = array->GetComponentTypeInfo();
        if (componentTypeInfo->IsStructType()) {
            GCTib gcTib = componentTypeInfo->GetGCTib();
            MAddress contentAddr = reinterpret_cast<Uptr>(array) + MArray::GetContentOffset();
            size_t elementSize = array->GetElementSize();
            for (MIndex i = 0; i < arrayLength; ++i) {
                gcTib.ForEachBitmapWord(contentAddr, visitor);
                contentAddr += elementSize;
            }
        } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
                   componentTypeInfo->IsInterface()) {
            HeapSlot<>* arrayContent = &HeapSlotAt<>(array->ConvertToCArray());
            for (MIndex i = 0; i < arrayLength; ++i) {
                visitor(arrayContent[i]);
            }
        } else {
            LOG(RTLOG_FATAL, "array object %p has wrong component type", array);
        }
        return;
    }

    MAddress contentAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    obj->GetGCTib().ForEachBitmapWord(contentAddr, visitor);
}

BaseObject* WCollector::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (is_mark_good(oldField)) {
        BaseObject* targetObj = to_object(oldField.GetTargetObject());
        if (!Collector::MarkGoodHeapGate("GetAndTryTagObj", targetObj)) {
            return nullptr;
        }
        // Anchor main ced6b14fe41380fd2dfb94c91b7fe6973786a80e
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by %s object %p: %s and offset %zd", targetObj, sourceKind, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
        return targetObj;
    }
    latest = make_load_good(oldField);
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by %s object %p: %s and offset %zd",
                 latest, sourceKind, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, raw(oldField.GetFieldValue()),
            raw(newField.GetFieldValue()), latest, latest->GetTypeInfo(), latest->GetSize());
    }
    return latest;
}

BaseObject* WCollector::ForwardUpdateRawRef(ObjectRef& root)
{
    zaddress_unsafe observed = root.LoadPlain();
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* oldObj = to_object(observedBits.GetTargetObject());
    DLOG(FIX, "visit raw-ref @%p: %p", &root, oldObj);
    // Static / RO slots (e.g. .data.rel.ro under GNU_RELRO) hold non-heap objects that
    // are never evacuated. Keep their existing plain value and skip write-back.
    // Same heap gate as IsGhostFromObject / FindToVersion / FixMinorEvacuatedSlot resolve.
    if (oldObj == nullptr || !Heap::IsHeapAddress(oldObj)) {
        return oldObj;
    }
    // arrayinit2 / markfloor Q2 / introot: stackmap may label RawArray+8 (&length) as a root.
    // Colouring that interior makes the mutator load a non-canonical address (si_code=128).
    // Relocate via host object; write plain interior (toHost+offset) back.
    if (!Collector::PlausibleManagedObjectGate("ForwardUpdateRawRef", oldObj)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(oldObj);
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            BaseObject* toHost = TryForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                    reinterpret_cast<uintptr_t>(toHost) +
                    (reinterpret_cast<uintptr_t>(oldObj) - reinterpret_cast<uintptr_t>(host)));
                HealRoot(root, from_object(toInterior));
                return toInterior;
            }
        }
        HealRoot(root, from_object(oldObj));
        return oldObj;
    }
    if (IsGhostFromObject(oldObj)) {
        BaseObject* toVersion = TryForwardObject(oldObj);
        CHECK(toVersion != nullptr);
        HealRoot(root, from_object(toVersion));
        DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, toVersion);
        return toVersion;
    } else {
        HealRoot(root, from_object(oldObj));
    }

    return oldObj;
}
void WCollector::PreforwardAllExportFromRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitAllExportRoots(visitor);
}
void WCollector::PreforwardStaticRoots()
{
    RootSlotVisitor visitor = [this](RootSlot& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitStaticRoots(visitor);
}
void WCollector::PreforwardFinalizerProcessorRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitor);
}

void WCollector::PreforwardConcurrencyModelRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void WCollector::PreforwardDiscoveredExternObjects()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    auto it = cycleRefWorkStack.begin();
    std::unordered_map<BaseObject*, std::list<BaseObject*>> tmp;
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        for (auto &externObj : it->second) {
            if (IsGhostFromObject(externObj) && !IsUnmovableFromObject(externObj)) {
                BaseObject* toObj = ForwardObject(externObj);
                externObj = toObj;
            }
        }
        if (latest != exportObj) {
            tmp[latest] = it->second;
            it = cycleRefWorkStack.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        cycleRefWorkStack.insert(tmp.begin(), tmp.end());
    }
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::unordered_set<BaseObject*> tmp;
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    auto it = resurrectedExportObjectes.begin();
    while (it != resurrectedExportObjectes.end()) {
        BaseObject* exportObj = *it;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        if (latest != exportObj) {
            tmp.insert(latest);
            it = resurrectedExportObjectes.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        resurrectedExportObjectes.insert(tmp.begin(), tmp.end());
    }
}
void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    // plaincensus Phase 1a: measure plain HeapSlots before major mark.
    RunPlainCensus("pre-major-mark", false);

    // Full collection starts young and old marking in the same pause, as
    // VM_ZMarkStartYoungAndOld::do_operation does (OpenJDK zGeneration.cpp:583-605).
    flip_young_mark_start();
    flip_old_mark_start();

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        DoEnumeration(workStack, foreignStack);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }
}

void WCollector::FixOldTaggedRefField(BaseObject* holder, RefField<>& field)
{
    RefField<> oldField(field);
    if (!IsOldPointer(oldField)) {
        return;
    }
    BaseObject* fromObj = to_object(oldField.GetTargetObject());
    BaseObject* latest = FindToVersion(fromObj);
    if (latest == nullptr) {
        latest = fromObj;
    }
    bool latestLive = false;
    if (Heap::IsHeapAddress(latest)) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(latest));
        latestLive = region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
                     latest->IsValidObject();
    }
    if (!latestLive) {
        // Dead one-gen-stale residue (common right after Flip of the just-tagged
        // generation, or remset residue). Null the slot instead of fail-closed:
        // F5 still guards major FindLatestVersion consumers.
        static std::atomic<size_t> g_f3DeadLogged{ 0 };
        size_t n = g_f3DeadLogged.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            VLOG(REPORT,
                 "[GCV2][F3-dead] holder=%p field=%p from=%p latest=%p — null slot",
                 holder, &field, fromObj, latest);
        }
        {
            const char* reason = "unknown";
            unsigned rtype = 0;
            int latestValid = -1;
            if (latest == nullptr) {
                reason = "latest_null";
            } else if (!Heap::IsHeapAddress(latest)) {
                reason = "latest_not_heap";
            } else {
                RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(latest));
                if (region == nullptr) {
                    reason = "region_null";
                } else if (region->IsFreeRegion()) {
                    reason = "region_free";
                    rtype = static_cast<unsigned>(region->GetRegionType());
                } else if (region->IsGarbageRegion()) {
                    reason = "region_garbage";
                    rtype = static_cast<unsigned>(region->GetRegionType());
                } else {
                    latestValid = latest->IsValidObject() ? 1 : 0;
                    reason = latestValid ? "valid_but_not_live" : "invalid_object";
                    rtype = static_cast<unsigned>(region->GetRegionType());
                }
            }
            size_t whyN = g_nullslotF3.load(std::memory_order_relaxed);
            if (NullslotProbeEnabled() && whyN < 64) {
                LOG(RTLOG_ERROR,
                    "[GCV2][nullslot][f3why] n=%zu reason=%s rtype=%u latestValid=%d "
                    "holder=%p field=%p from=%p latest=%p",
                    whyN, reason, rtype, latestValid, holder, &field, fromObj, latest);
            }
        }
        NoteNullslotWrite("f3_fix_oldtag", holder, &field, fromObj, latest, &g_nullslotF3);
        RefField<> nullField(nullptr);
        (void)field.CompareExchange(oldField.GetFieldValue(), nullField.GetFieldValue());
        return;
    }
    // Phase C heap: write the current colour back, not a bare pointer.
    // plainroots non-heap root slots: write plain latest (ZGC uncolored root heal).
    //
    // The old comment here read "Always write a plain pointer... Re-tagging a still-from survivor
    // as current recreates the next generation of one-gen-stale after Flip". That was true while a
    // tag meant "mid-evacuation": re-tagging did manufacture a stale reference for the next cycle.
    // With a colour it is the opposite -- writing the current colour is what makes this reference
    // survive the next flip's test, and writing a bare pointer would put back the very trust state
    // this phase removes. This is the self-heal half of the barrier, the same shape as ZGC's
    // self_heal (jdk zBarrier.inline.hpp:330-340), except we already had the resolve step.
    RefField<> newField = RootSlotWriteback(latest, &field);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        return;
    }
    if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(FIX, "F3 fix old-tag holder %p field@%p: %#zx => %#zx -> %p", holder, &field,
             raw(oldField.GetFieldValue()), raw(newField.GetFieldValue()), latest);
    }
}

void WCollector::InvalidateOldTaggedRefsBeforeDispel()
{
    // A1 (preflipacc §5 / rspec §四 A1): production skips the preflip full-heap walk.
    // Population was empty across 123 majors × 3 loads; cost was ~27% of major total_gc.
    // VERIFY / ACCOUNT still force the walk so soak/ALOT can keep the insurance tripwire.
    static const bool preflipWalk = []() {
        const char* verify = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        if (verify != nullptr && std::strcmp(verify, "1") == 0) {
            return true;
        }
        const char* account = std::getenv("MRT_GCV2_PREFLIP_ACCOUNT");
        return account != nullptr && std::strcmp(account, "1") == 0;
    }();
    if (!preflipWalk) {
        return;
    }
    InvalidateOldTaggedRefs(true);
}

void WCollector::InvalidateOldTaggedRefs(bool requireSurvivedMark)
{
    MRT_PHASE_TIMER(requireSurvivedMark ? "InvalidateOldTaggedRefs.preflip" : "InvalidateOldTaggedRefs.postflip");
    ScopedStopTheWorld stw(requireSurvivedMark ? "invalidate old tagged refs before dispel"
                                               : "invalidate old tagged refs after flip");

    // A2: parallel full-heap STW walk (ops/design/REMSET_OPTION1_SPEC_0805.txt §六).
    // Sharding = atomic address cursor + region-head ownership; roots = 6 family tasks;
    // account counters are per-worker then merged (H1/H2/H3). A1 VERIFY/ACCOUNT locals below.
    static const bool accountEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_ACCOUNT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    static const bool preflipVerifyEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    static const bool preflipInjectEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY_INJECT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    // Locals for lambda capture (static const cannot be captured under -std=gnu++14).
    const bool account = accountEnv;
    const bool preflipVerify = preflipVerifyEnv;
    const bool preflipInject = preflipInjectEnv;
    // VERIFY always needs fixed counts so a non-zero residue can fail loud.
    const bool trackFixed = account || (requireSurvivedMark && preflipVerify);

    constexpr size_t regionTypeCount = static_cast<size_t>(RegionInfo::RegionType::GARBAGE_REGION) + 1;
    // CHUNK = 256 units × UNIT_SIZE (16MiB @ 64KB unit). Spec §六 T1.
    constexpr size_t kChunkUnits = 256;
    const size_t chunkBytes = kChunkUnits * RegionInfo::UNIT_SIZE;

    struct RootAccount {
        size_t rootSlots = 0;
        size_t oldTaggedRootSlots = 0;
        size_t fixedRootSlots = 0;
    };
    struct HeapAccount {
        size_t processedRegions = 0;
        size_t processedObjects = 0;
        size_t invalidObjects = 0;
        size_t filteredObjects = 0;
        size_t refHolders = 0;
        size_t fields = 0;
        size_t oldTaggedSlots = 0;
        size_t fixedSlots = 0;
        size_t youngTargetSlots = 0;
        size_t fromLiveObjects = 0;
        size_t fromLiveFields = 0;
        size_t rebuilt = 0;
        size_t chunksTaken = 0;
    };

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& regionManager = space.GetRegionManager();
    RememberedSet* rebuildRemset = requireSurvivedMark ? nullptr : &Heap::GetHeap().GetRememberedSet();
    const uintptr_t heapStart = regionManager.GetRegionHeapStart();
    const uintptr_t inactiveZone = regionManager.GetInactiveZone();

    auto makeRootVisitor = [this, trackFixed](RootAccount* acc) -> RootVisitor {
        return [this, trackFixed, acc](ObjectRef& root) {
            uintptr_t oldValue = raw(root.LoadPlain());
            HeapSlot<> observedBits(to_zpointer(oldValue));
            bool oldTagged = trackFixed && IsOldPointer(observedBits);
            if (trackFixed && acc != nullptr) {
                ++acc->rootSlots;
                if (oldTagged) {
                    ++acc->oldTaggedRootSlots;
                }
            }
            ForwardUpdateRawRef(root);
            if (trackFixed && acc != nullptr && oldTagged && raw(root.LoadPlain()) != oldValue) {
                ++acc->fixedRootSlots;
            }
        };
    };

    auto processObject = [this, requireSurvivedMark, rebuildRemset, account, trackFixed](BaseObject* obj,
                                                                                          HeapAccount& acc) {
        RegionInfo* accountRegion = nullptr;
        if (account) {
            ++acc.processedObjects;
            if (obj != nullptr) {
                accountRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            }
            // H2: region count is per-worker local; each region is owned by exactly one
            // worker (region-head ownership), so summing processedRegions is exact.
        }
        if (obj == nullptr || !obj->IsValidObject()) {
            if (account) {
                ++acc.invalidObjects;
            }
            return;
        }
        if (requireSurvivedMark) {
            if (!IsSurvivedObject(obj)) {
                if (account) {
                    ++acc.filteredObjects;
                }
                return;
            }
            if (account && accountRegion != nullptr && accountRegion->IsFromRegion()) {
                ++acc.fromLiveObjects;
            }
        }
        if (!obj->HasRefField()) {
            return;
        }
        if (account) {
            ++acc.refHolders;
        }
        bool recordCrossGen = false;
        if (rebuildRemset != nullptr) {
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            recordCrossGen = holderRegion != nullptr && !holderRegion->IsYoungRegion() &&
                             !holderRegion->IsGarbageRegion() && !holderRegion->IsFreeRegion();
        }
        bool forwardHolder = account && requireSurvivedMark && accountRegion != nullptr &&
                             accountRegion->IsFromRegion();
        obj->ForEachRefField([this, obj, recordCrossGen, rebuildRemset, forwardHolder, account, trackFixed,
                              &acc](RefField<>& field) {
            uintptr_t oldValue = raw(field.GetFieldValue());
            bool oldTagged = trackFixed && IsOldPointer(field);
            if (trackFixed) {
                ++acc.fields;
                if (forwardHolder) {
                    ++acc.fromLiveFields;
                }
                if (oldTagged) {
                    ++acc.oldTaggedSlots;
                }
            }
            FixOldTaggedRefField(obj, field);
            if (oldTagged && raw(field.GetFieldValue()) != oldValue) {
                ++acc.fixedSlots;
            }
            if (!recordCrossGen) {
                return;
            }
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                if (account) {
                    ++acc.youngTargetSlots;
                }
                rebuildRemset->Record(reinterpret_cast<MAddress>(&field));
                ++acc.rebuilt;
            }
        });
    };

    // Region-head-ownership walk over [rangeStart, rangeEnd). H6: carry transient-extent
    // guard verbatim. Spec §六 T1: first-step correction if region head is before rangeStart.
    auto walkRange = [&processObject, requireSurvivedMark, account](uintptr_t rangeStart, uintptr_t rangeEnd,
                                                                    uintptr_t inactive, HeapAccount& acc) {
        if (rangeStart >= rangeEnd || rangeStart >= inactive) {
            return;
        }
        uintptr_t limit = std::min(rangeEnd, inactive);
        uintptr_t addr = rangeStart;
        // First-step correction: if the unit at rangeStart is mid-region, skip to that
        // region's end — the region belongs to the worker that owns its head.
        {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t regionStart = region->GetRegionStart();
            uintptr_t nextAddr = region->GetRegionEnd();
            if (nextAddr <= addr || nextAddr > inactive) {
                // Transient illegal extent: step one unit, do not visit (H6).
                addr += RegionInfo::UNIT_SIZE;
            } else if (regionStart < rangeStart) {
                addr = nextAddr;
            }
        }
        RegionInfo* lastProcessedRegion = nullptr;
        while (addr < limit) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t nextAddr = region->GetRegionEnd();
            // H6 transient-extent guard — character-identical to ForEachObjUnsafe.
            if (nextAddr <= addr || nextAddr > inactive) {
                addr += RegionInfo::UNIT_SIZE;
                continue;
            }
            // Region-head ownership: only visit if the region's head is in this chunk.
            // Regions that spill past limit still belong entirely to this worker.
            if (addr >= rangeStart && addr < limit) {
                if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
                    !(requireSurvivedMark && region->IsKnownEmpty())) {
                    if (account && region != lastProcessedRegion) {
                        lastProcessedRegion = region;
                        ++acc.processedRegions;
                    }
                    region->VisitAllObjects([&processObject, &acc](BaseObject* object) {
                        processObject(object, acc);
                    });
                }
            }
            addr = nextAddr;
        }
    };

    auto heapWorkerBody = [&](std::atomic<uintptr_t>& cursor, HeapAccount& acc) {
        for (;;) {
            uintptr_t chunkStart = cursor.fetch_add(chunkBytes, std::memory_order_relaxed);
            if (chunkStart >= inactiveZone) {
                break;
            }
            ++acc.chunksTaken;
            uintptr_t chunkEnd = chunkStart + chunkBytes;
            walkRange(chunkStart, chunkEnd, inactiveZone, acc);
        }
    };

    // H7: account shadow pass stays serial (diagnostic-only, not on hot path).
    std::array<size_t, regionTypeCount> regionTypes{};
    size_t regions = 0;
    size_t knownEmptyRegions = 0;
    size_t objects = 0;
    size_t knownEmptyObjects = 0;
    size_t fromRegions = 0;
    if (account) {
        RegionInfo* lastAccountRegion = nullptr;
        space.ForEachObj(
            [requireSurvivedMark, &regionTypes, &lastAccountRegion, &regions, &knownEmptyRegions, &objects,
             &knownEmptyObjects, &fromRegions](BaseObject* obj) {
                ++objects;
                RegionInfo* region = obj == nullptr ? nullptr :
                    RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region == nullptr) {
                    return;
                }
                if (region != lastAccountRegion) {
                    lastAccountRegion = region;
                    ++regions;
                    ++regionTypes[static_cast<size_t>(region->GetRegionType())];
                    if (requireSurvivedMark && region->IsKnownEmpty()) {
                        ++knownEmptyRegions;
                    }
                    if (requireSurvivedMark && region->IsFromRegion()) {
                        ++fromRegions;
                    }
                }
                if (requireSurvivedMark && region->IsKnownEmpty()) {
                    ++knownEmptyObjects;
                }
            },
            false);
    }

    // A1 positive control: plant one old-tagged root so PREFLIP_VERIFY must observe fixed>0.
    // Serial, before parallel dispatch; only on preflip path.
    RootAccount injectAcc{};
    if (requireSurvivedMark && preflipVerify && preflipInject) {
        BaseObject* injectTarget = nullptr;
        space.ForEachObj(
            [&injectTarget](BaseObject* obj) {
                if (injectTarget != nullptr || obj == nullptr || !obj->IsValidObject()) {
                    return;
                }
                injectTarget = obj;
            },
            false);
        if (injectTarget != nullptr) {
            RefField<> planted(injectTarget, 1, GetPreviousTagID());
            MAddress injectRootStorage = raw(planted.GetFieldValue());
            ObjectRef injectRoot{};
            *reinterpret_cast<MAddress*>(&injectRoot) = injectRootStorage;
            RootVisitor fixRoot = makeRootVisitor(&injectAcc);
            fixRoot(injectRoot);
            VLOG(REPORT,
                 "[GCV2][preflip-verify-inject] planted old-tag root target=%p raw=%#zx fixedRoots_now=%zu "
                 "env=MRT_GCV2_PREFLIP_VERIFY_INJECT=1",
                 injectTarget, static_cast<uintptr_t>(injectRootStorage), injectAcc.fixedRootSlots);
        } else {
            VLOG(REPORT,
                 "[GCV2][preflip-verify-inject] no live object to plant env=MRT_GCV2_PREFLIP_VERIFY_INJECT=1");
        }
    }

    GCThreadPool* threadPool = GetThreadPool();
    // Positive control for silent serial degradation (spec §六 T3 ②).
    // Force serial via MRT_GCV2_STWPAR_FORCE_SERIAL=1 for bidirectional proof.
    static const bool forceSerialEnv = []() {
        const char* value = std::getenv("MRT_GCV2_STWPAR_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool forceSerial = forceSerialEnv;
    const bool useParallel = threadPool != nullptr && !forceSerial;

    RootAccount rootTotals = injectAcc;
    HeapAccount heapTotals{};
    std::vector<size_t> chunksPerWorker;
    size_t workersScheduled = 0;

    if (!useParallel) {
        VLOG(REPORT, "[F3][parallel] fallback=serial pool_unavailable");
        // Six root families serial (same order as before).
        {
            RootAccount acc;
            RootVisitor fixRoot = makeRootVisitor(&acc);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
            Heap::GetHeap().VisitStaticRoots(fixRoot);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
            rootTotals.rootSlots += acc.rootSlots;
            rootTotals.oldTaggedRootSlots += acc.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += acc.fixedRootSlots;
        }
        {
            HeapAccount acc;
            // Single-threaded full range — equivalent to ForEachObjUnsafe.
            walkRange(heapStart, inactiveZone, inactiveZone, acc);
            // Count as one logical chunk for the diagnostic line.
            if (heapStart < inactiveZone) {
                acc.chunksTaken = 1;
            }
            heapTotals = acc;
            chunksPerWorker.push_back(acc.chunksTaken);
            workersScheduled = 1;
        }
    } else {
        // Root-side: 6 family-level tasks (static family must not be split — mutex+dedup set).
        // Heap-side: N cursor tasks. Same pool, same batch as Preforward.
        // Cap via MRT_GCV2_STWPAR_WORKERS for scale curve (1/2/4/8/16); never expand pool.
        const int32_t helperNum = threadPool->GetMaxThreadNum();
        // Caller's GC thread also drains via WaitFinish → effective capacity = helpers + 1.
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* wEnv = std::getenv("MRT_GCV2_STWPAR_WORKERS");
            if (wEnv != nullptr && wEnv[0] != '\0') {
                int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
                if (want >= 1 && want < heapWorkers) {
                    heapWorkers = want;
                }
            }
        }
        std::vector<RootAccount> rootAcc(6);
        std::vector<HeapAccount> heapAcc(static_cast<size_t>(heapWorkers));
        std::atomic<uintptr_t> cursor{ heapStart };

        // Roots first into queue, then heap workers. Start after all AddWork so helpers
        // see the full batch (same shape as Preforward: AddWork×N then Start then WaitFinish).
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[0]);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[1]);
            Heap::GetHeap().VisitStaticRoots(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[2]);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[3]);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[4]);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[5]);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
        }));

        for (int32_t i = 0; i < heapWorkers; ++i) {
            HeapAccount* acc = &heapAcc[static_cast<size_t>(i)];
            threadPool->AddWork(new (std::nothrow) LambdaWork(
                [heapWorkerBody, &cursor, acc](size_t) { heapWorkerBody(cursor, *acc); }));
        }

        threadPool->Start();
        threadPool->WaitFinish();

        for (const auto& a : rootAcc) {
            rootTotals.rootSlots += a.rootSlots;
            rootTotals.oldTaggedRootSlots += a.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += a.fixedRootSlots;
        }
        chunksPerWorker.reserve(heapAcc.size());
        for (const auto& a : heapAcc) {
            heapTotals.processedRegions += a.processedRegions;
            heapTotals.processedObjects += a.processedObjects;
            heapTotals.invalidObjects += a.invalidObjects;
            heapTotals.filteredObjects += a.filteredObjects;
            heapTotals.refHolders += a.refHolders;
            heapTotals.fields += a.fields;
            heapTotals.oldTaggedSlots += a.oldTaggedSlots;
            heapTotals.fixedSlots += a.fixedSlots;
            heapTotals.youngTargetSlots += a.youngTargetSlots;
            heapTotals.fromLiveObjects += a.fromLiveObjects;
            heapTotals.fromLiveFields += a.fromLiveFields;
            heapTotals.rebuilt += a.rebuilt;
            heapTotals.chunksTaken += a.chunksTaken;
            chunksPerWorker.push_back(a.chunksTaken);
        }
        workersScheduled = static_cast<size_t>(heapWorkers);
    }

    // Parallel-liveness positive control: at least 2 workers with chunks_taken>0 when heap
    // spans >2×CHUNK (spec §六 T3 ①). Always print so silence ≠ "never fired".
    {
        size_t active = 0;
        std::string chunksStr;
        for (size_t i = 0; i < chunksPerWorker.size(); ++i) {
            if (chunksPerWorker[i] != 0) {
                ++active;
            }
            if (i != 0) {
                chunksStr += ',';
            }
            chunksStr += std::to_string(chunksPerWorker[i]);
        }
        VLOG(REPORT, "[F3][parallel] phase=%s workers_active=%zu workers_scheduled=%zu chunks=[%s] parallel=%d",
             requireSurvivedMark ? "preflip" : "postflip", active, workersScheduled, chunksStr.c_str(),
             useParallel ? 1 : 0);
    }

    if (heapTotals.rebuilt != 0) {
        VLOG(REPORT, "[GCV2][remset] rebuilt after full GC recorded=%zu", heapTotals.rebuilt);
    }
    if (account) {
        VLOG(REPORT,
             "[GCV2][preflip-account] phase=%s regions=%zu knownEmptyRegions=%zu objects=%zu "
             "knownEmptyObjects=%zu processedRegions=%zu processedObjects=%zu invalid=%zu filtered=%zu "
             "survived=%zu refHolders=%zu fields=%zu oldTagged=%zu fixed=%zu rootSlots=%zu "
             "oldTaggedRoots=%zu fixedRoots=%zu youngTargets=%zu "
             "rebuilt=%zu fromRegions=%zu fromLiveObjects=%zu fromLiveFields=%zu "
             "env=MRT_GCV2_PREFLIP_ACCOUNT=1",
             requireSurvivedMark ? "preflip" : "postflip", regions, knownEmptyRegions, objects, knownEmptyObjects,
             heapTotals.processedRegions, heapTotals.processedObjects, heapTotals.invalidObjects,
             heapTotals.filteredObjects,
             heapTotals.processedObjects - heapTotals.invalidObjects - heapTotals.filteredObjects,
             heapTotals.refHolders, heapTotals.fields, heapTotals.oldTaggedSlots, heapTotals.fixedSlots,
             rootTotals.rootSlots, rootTotals.oldTaggedRootSlots, rootTotals.fixedRootSlots,
             heapTotals.youngTargetSlots, heapTotals.rebuilt, fromRegions, heapTotals.fromLiveObjects,
             heapTotals.fromLiveFields);
        VLOG(REPORT,
             "[GCV2][preflip-region-types] phase=%s type0=%zu type1=%zu type2=%zu type3=%zu type4=%zu "
             "type5=%zu type6=%zu type7=%zu type8=%zu type9=%zu type10=%zu type11=%zu type12=%zu type13=%zu "
             "type14=%zu env=MRT_GCV2_PREFLIP_ACCOUNT=1",
             requireSurvivedMark ? "preflip" : "postflip", regionTypes[0], regionTypes[1], regionTypes[2],
             regionTypes[3], regionTypes[4], regionTypes[5], regionTypes[6], regionTypes[7], regionTypes[8],
             regionTypes[9], regionTypes[10], regionTypes[11], regionTypes[12], regionTypes[13], regionTypes[14]);
    }
    // A1 VERIFY tripwire: preflip fixed population must stay empty. Any fix is a named failure.
    if (requireSurvivedMark && preflipVerify) {
        const size_t fixedTotal = heapTotals.fixedSlots + rootTotals.fixedRootSlots;
        VLOG(REPORT,
             "[GCV2][preflip-verify] fixed=%zu fixedRoots=%zu fixedTotal=%zu oldTagged=%zu oldTaggedRoots=%zu "
             "fields=%zu rootSlots=%zu env=MRT_GCV2_PREFLIP_VERIFY=1",
             heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
             rootTotals.oldTaggedRootSlots, heapTotals.fields, rootTotals.rootSlots);
        if (fixedTotal > 0) {
            static const bool preflipVerifyFatal = []() {
                const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY_FATAL");
                return value != nullptr && std::strcmp(value, "1") == 0;
            }();
            LOG(RTLOG_ERROR,
                "[GCV2][preflip-verify] PREFLIP_RESIDUE fixed=%zu fixedRoots=%zu fixedTotal=%zu "
                "oldTagged=%zu oldTaggedRoots=%zu (production skips preflip; residue means insurance needed)",
                heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
                rootTotals.oldTaggedRootSlots);
            if (preflipVerifyFatal) {
                CHECK_DETAIL(false,
                             "MRT_GCV2_PREFLIP_VERIFY_FATAL: preflip residue fixedTotal=%zu "
                             "(fixed=%zu fixedRoots=%zu)",
                             fixedTotal, heapTotals.fixedSlots, rootTotals.fixedRootSlots);
            }
        }
    }
}

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER("PostTrace");
    TransitionToGCPhase(GC_PHASE_POST_TRACE, true);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.GetRegionManager().HandleTraceRegions();
    // clear weakRef List, set the referent as null
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    // clear satb buffer when gc finish tracing.
    SatbBuffer::Instance().ClearBuffer();
    // reclaim large objects immediately after tracing is done.
    PrepareCycleRef();
    CollectLargeGarbage();
    CollectPinnedGarbage();
    RefineFromSpace();
    // F3: dispel previous ghost from-regions next; kill one-gen-stale tags first so
    // IsOldPointer cannot outlive FindToVersion's ghost gate (D phase).
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
    InvalidateOldTaggedRefsBeforeDispel();
    fwdTable.PrepareForwardTable();
    // OPTION_2 mark-epoch release: TRACE+CLEAR_SATB done; publish quarantined post-dispel
    // units (from this PrepareForwardTable and any prior minor) to dirty for reuse.
    // INV-1 closed: concurrent mark can no longer follow plain edges into these ranges.
    space.GetRegionManager().ReleaseMarkQuarantine();
}

void WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER("Preforward");
    {
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
        // This collector relocates both generations in one full-GC relocation set. Match the two
        // generation relocate-start flips while mutators are stopped, before any root is forwarded.
        flip_young_relocate_start();
        flip_old_relocate_start();
    }

    GCThreadPool* threadPool = GetThreadPool();
    MRT_ASSERT(threadPool != nullptr, "thread pool is null");
    // forward and fix cj future objects
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardConcurrencyModelRoots(); }));

    // forward and fix finalizer roots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardFinalizerProcessorRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllExportFromRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardStaticRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));
    threadPool->Start();
    threadPool->WaitFinish();
}


extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(Heap::GetBarrier().ReadReference(this,
            HeapSlotAt<false>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(Heap::GetBarrier().ReadReference(this,
            HeapSlotAt<false>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler WCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__)
    size_t i = 0;
    if (!cycleWorkStackMtx.try_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    for (auto it = cycleRefWorkStack.begin(); it != cycleRefWorkStack.end(); i++) {
        ScopedObjectAccess soa;
        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleWorkStackMtx.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }
        BaseObject* exportObj = it->first;
        auto& heap = Heap::GetHeap();
        auto id = static_cast<ExportObject*>(exportObj)->GetId();
        if (!heap.CheckExportObjState(id, exportObj)) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        if (resurrectedExportObjectes.find(exportObj) != resurrectedExportObjectes.end() ||
            resurrectedExportObjectesForwardPhase.find(exportObj) != resurrectedExportObjectesForwardPhase.end()) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        auto externObjs = it->second;
        void* returnUnit = nullptr;
        for (auto externObj : externObjs) {
            auto resolveHook = GetCrossRefHandler(externObj);
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
        }
        heap.SetExportObjActiveState(id, false);
        it++;
    }
    cycleWorkStackMtx.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void WCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}

// N2 (MINOR_CONCURRENCY_0805 §八 T-C): CAS-install resolved target under multi-worker fix.
// Same-value concurrent writes converge; first writer wins. Counters for positive control.
namespace {
std::atomic<size_t> g_minorRefCasFail{ 0 };
std::atomic<size_t> g_minorRefCasOk{ 0 };

// Install resolved target into field. expected = observed stale/old value.
// On CAS fail: accept (peer already updated — major TryUpdateRefFieldImpl style).
// hangfloor: desired must already be RootSlotWriteback / null — heap slots stay coloured
// (Phase C); only non-heap root slots may be plain. Writing plain into a heap field
// installs the trust state is_load_good rejects, and every subsequent barrier self-heal
// turns young GC into thrash (arm A' 10/10 HANG under MRT_GCV2_PLAIN_ROOTS=1).
bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, RefField<> desired)
{
    MAddress desiredVal = raw(desired.GetFieldValue());
    if (expected == desiredVal) {
        return true;
    }
    if (field.CompareExchange(to_zpointer(expected), to_zpointer(desiredVal))) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// installdomain (ZGC mark_and_remember shape, GC-thread side): before installing a
// from/ghost-from address into a heap slot (or forwarding it), ensure the survivor
// bit that GetRoute will read is set.
//
// Two windows:
//   (1) pass1 before PrepareForwardable: region is still from (not yet ghost). Mark
//       current liveInfo; PrepareForwardable does liveInfo0 = liveInfo (pointer copy)
//       so the paint is snapshotted into the route domain.
//   (2) after PrepareForwardable while routeState==FORWARDABLE: MarkObject writes the
//       same LiveInfo that liveInfo0 points at — visible to GetRoute, not wiped by
//       ClearLiveInfo (that already ran at PrepareYoung).
// After ROUTED, liveByteCount/geometry are frozen — do not paint (tooLate counter).
void EnsureRouteDomainMembership(WCollector* collector, BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("EnsureRouteDomain", obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!obj->IsValidObject()) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (collector->IsUnmovableFromObject(obj)) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool isGhost = collector->IsGhostFromObject(obj);
    const bool isFrom = collector->IsFromObject(obj);
    if (!isGhost && !isFrom) {
        g_installDomainSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    // Prefer ghost face when present (what GetRoute reads); else current liveInfo.
    LiveInfo* face = region->GetLiveInfo0ForProbe();
    if (face == nullptr) {
        face = region->GetLiveInfo();
    }
    if (face != nullptr && face->IsSurvivedObject(offset)) {
        g_installDomainAlready.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (isGhost) {
        // Only paint while FORWARDABLE: RouteOrCompactRegionImpl freezes liveByteCount.
        RegionInfo::RouteState rs = region->GetRouteState();
        if (rs != RegionInfo::RouteState::FORWARDABLE) {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Mark current liveInfo (post-snapshot: same pointer as liveInfo0 when non-null).
    (void)collector->MarkObject(obj);
    // If ghost face was null (snapshot of empty liveInfo), bind freshly allocated liveInfo
    // so GetRoute's liveInfo0!=null gate opens on the bits we just painted.
    if (isGhost) {
        region->BindLiveInfo0FromLiveIfNull();
    }
    LiveInfo* live = region->GetLiveInfo();
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    if (ghost != nullptr && ghost != live && ghost->markBitmap != nullptr &&
        reinterpret_cast<uintptr_t>(ghost->markBitmap) != LiveInfo::TEMPORARY_PTR) {
        size_t objSize = obj->GetSize();
        MAddress regionStart = region->GetRegionStart();
        size_t regionSize = static_cast<size_t>(region->GetRegionEnd() - regionStart);
        if (objSize > 0 && offset + objSize <= regionSize) {
            (void)ghost->markBitmap->MarkBits(offset, objSize, regionSize);
        }
    }
    // Re-check: grant only counts if GetRoute face now accepts (positive control truth).
    ghost = region->GetLiveInfo0ForProbe();
    if (isGhost) {
        if (ghost != nullptr && ghost->IsSurvivedObject(offset)) {
            g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_installDomainTooLate.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // pre-snapshot from: paint lands on liveInfo; PrepareForwardable will copy pointer.
        g_installDomainGrant.fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace

BaseObject* WCollector::ResolveMinorReference(RefField<>& field) const
{
    RefField<> value(field);
    BaseObject* object = to_object(value.GetTargetObject());
    if (!IsOldPointer(value)) {
        // hangfloor: plain stack/reg roots (and any load-good colour) make IsOldPointer
        // structurally false — that predicate needs IsLoadBad, which plain never is.
        // After young prepare, from-space still needs ghost routing; without it
        // FixMinor/VisitMinor keep the from address and young GC thrash (10/10 HANG).
        if (object != nullptr && Heap::IsHeapAddress(object) && IsGhostFromObject(object) &&
            !IsUnmovableFromObject(object)) {
            // installdomain: admit into route domain before any install/forward consumes it.
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            BaseObject* to = FindToVersion(object);
            if (to != nullptr && Heap::IsHeapAddress(to)) {
                MAddress expected = raw(value.GetFieldValue());
                (void)CasInstallResolvedTarget(field, expected, RootSlotWriteback(to, &field));
                return to;
            }
        }
        return object;
    }
    // Minor path must not call FindLatestVersion: after a full GC Flip, remset/root
    // slots can still hold one-gen-stale tags whose from-copy was reclaimed (ghost
    // gone, header zeroed). F5 would abort a detector path; here we soft-resolve:
    //   routed to-version → RootSlotWriteback(to)
    //   unmoved valid from → RootSlotWriteback(from)
    //   dead/stale → null the slot (caller drops the edge)
    // N2: CAS (FYS=1 multi-writer safe; product default FYS=1).
    // hangfloor: use RootSlotWriteback so heap remset/fields keep Phase C colour.
    MAddress expected = raw(value.GetFieldValue());
    BaseObject* to = FindToVersion(object);
    if (to != nullptr && Heap::IsHeapAddress(to)) {
        RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
        if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
            to->IsValidObject()) {
            (void)CasInstallResolvedTarget(field, expected, RootSlotWriteback(to, &field));
            return to;
        }
    }
    if (Heap::IsHeapAddress(object)) {
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
            object->IsValidObject()) {
            // installdomain: identity arm is the A-only fork (IsValidObject without liveInfo0).
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            (void)CasInstallResolvedTarget(field, expected, RootSlotWriteback(object, &field));
            return object;
        }
    }
    // Non-heap (static/binary constants, etc.): FindToVersion nullptr means "not a heap
    // object", not "dead residue". Return as-is; never CAS (slot may be RO static root).
    // See reports/REPORT-zcdnull.md — CAS-null on RO static SEGV (si_addr=&field).
    if (object != nullptr && !Heap::IsHeapAddress(object)) {
        return object;
    }
    static std::atomic<size_t> g_staleOldTagLogged{ 0 };
    size_t n = g_staleOldTagLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        VLOG(REPORT,
             "[GCV2][minor-stale-oldtag] field=%p raw=%#zx from=%p to=%p "
             "(drop; full-GC remset/root residue after Flip)",
             &field, static_cast<size_t>(raw(value.GetFieldValue())), object, to);
    }
    NoteNullslotWrite("fix_resolve_cas", nullptr, &field, object, to, &g_nullslotResolve);
    (void)CasInstallResolvedTarget(field, expected, RefField<>(nullptr));
    return nullptr;
}

BaseObject* WCollector::ResolveMinorReference(RootSlot& root) const
{
    zaddress_unsafe observed = root.LoadPlain();
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* object = to_object(observedBits.GetTargetObject());
    if (!IsOldPointer(observedBits)) {
        if (object != nullptr && Heap::IsHeapAddress(object) && IsGhostFromObject(object) &&
            !IsUnmovableFromObject(object)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            BaseObject* to = FindToVersion(object);
            if (to != nullptr && Heap::IsHeapAddress(to)) {
                HealRoot(root, from_object(to));
                return to;
            }
        }
        return object;
    }

    BaseObject* to = FindToVersion(object);
    if (to != nullptr && Heap::IsHeapAddress(to)) {
        RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
        if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
            to->IsValidObject()) {
            HealRoot(root, from_object(to));
            return to;
        }
    }
    if (Heap::IsHeapAddress(object)) {
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
            object->IsValidObject()) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            HealRoot(root, from_object(object));
            return object;
        }
    }
    if (object != nullptr && !Heap::IsHeapAddress(object)) {
        return object;
    }
    static std::atomic<size_t> g_staleOldRootLogged{ 0 };
    size_t n = g_staleOldRootLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        VLOG(REPORT,
             "[GCV2][minor-stale-oldtag] root=%p raw=%#zx from=%p to=%p "
             "(drop; full-GC root residue after Flip)",
             &root, static_cast<size_t>(raw(observed)), object, to);
    }
    HealRoot(root, zaddress::null);
    return nullptr;
}

namespace {
// gcbadroot: tag which root family is currently being walked so PushYoungObject
// can attribute invalid headers without threading origin through every visitor.
thread_local const char* gMinorRootOrigin = "unknown";
} // namespace

void WCollector::VisitMinorRootSlots(RootVisitor& rawRootVisitor, uint64_t stackScanEpoch)
{
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    RootVisitor checkedRawRootVisitor = [&remset, &rawRootVisitor](ObjectRef& root) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&root));
        rawRootVisitor(root);
    };
    RootVisitor& visitedRawRootVisitor = checkedRawRootVisitor;
#else
    RootVisitor& visitedRawRootVisitor = rawRootVisitor;
#endif
    gMinorRootOrigin = "mutator_stack";
    size_t concurrentDone = 0;
    size_t stwFallback = 0;
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        if (stackScanEpoch != 0 && mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
            ++concurrentDone;
            return;
        }
        if (stackScanEpoch != 0) {
            ++stwFallback;
        }
        mutator.VisitMutatorRoots(visitedRawRootVisitor);
    });
    if (stackScanEpoch != 0) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-scan-fallback] epoch=%llu concurrent_done=%zu stw_fallback=%zu "
            "env=MRT_GCV2_CONCURRENT_STACK_SCAN=1",
            static_cast<unsigned long long>(stackScanEpoch), concurrentDone, stwFallback);
    }
    gMinorRootOrigin = "static";
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    Heap::GetHeap().VisitStaticRoots([&remset, &visitedRawRootVisitor](RootSlot& root) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&root));
        visitedRawRootVisitor(root);
    });
#else
    Heap::GetHeap().VisitStaticRoots(visitedRawRootVisitor);
#endif
    gMinorRootOrigin = "concurrency";
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitedRawRootVisitor);
    gMinorRootOrigin = "finalizer";
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitedRawRootVisitor);
    gMinorRootOrigin = "export";
    Heap::GetHeap().VisitAllExportRoots(visitedRawRootVisitor);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    remset.CheckStaticCoverageForMinor();
#endif
    gMinorRootOrigin = "unknown";
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        gMinorRootOrigin = "value_export";
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        gMinorRootOrigin = "value_export_fwd";
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    gMinorRootOrigin = "value_cycle";
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
    gMinorRootOrigin = "unknown";
}

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor, uint64_t stackScanEpoch)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        BaseObject* obj = ResolveMinorReference(root);
        if (obj != nullptr && Heap::IsHeapAddress(obj) &&
            !Collector::PlausibleManagedObjectGate("VisitMinorRoots.raw", obj)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(obj);
            if (host != nullptr) {
                visitor(host);
            }
            return;
        }
        visitor(obj);
    };
    VisitMinorRootSlots(rawRootVisitor, stackScanEpoch);
    VisitMinorValueRoots(visitor);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    // markfloor / introot: interiors (RawArray+8) pass IsValidObject (tip=length≠null).
    // Recover host object so the live array is marked; do not push the interior itself.
    if (!Collector::PlausibleManagedObjectGate("PushYoungObject", object)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        if (host != nullptr && host != object) {
            PushYoungObject(host, workStack, origin);
        }
        return;
    }
    if (!object->IsValidObject()) {
        // Rich diagnosis before fail-closed abort: address looks like a heap range
        // but object header is not a valid managed object (stack-ish residue, stale
        // slot, or stackmap-mislabeled root). Printed once per process by default.
        static std::atomic<size_t> g_invalidMinorRootPrinted{ 0 };
        size_t n = g_invalidMinorRootPrinted.fetch_add(1, std::memory_order_relaxed);
        // Prefer explicit non-generic origin; "minor_root" is a placeholder that
        // should yield to the TLS tag set by VisitMinorRootSlots/ValueRoots.
        const char* src = origin;
        if (src == nullptr || std::strcmp(src, "unknown") == 0 || std::strcmp(src, "minor_root") == 0) {
            if (gMinorRootOrigin != nullptr && std::strcmp(gMinorRootOrigin, "unknown") != 0) {
                src = gMinorRootOrigin;
            } else if (src == nullptr) {
                src = "unknown";
            }
        }
        if (n < 8) {
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
            VLOG(REPORT,
                 "[GCV2][invalid-minor-root] obj=%p origin=%s region=%p regionStart=%#zx young=%u pinned=%u "
                 "large=%u free=%u garbage=%u neverExamined=%u "
                 "(fail-closed next; AS1 relation: bad header on stack-live slot vs SKIPPED frame)",
                 object, src, region,
                 region == nullptr ? 0 : static_cast<size_t>(region->GetRegionStart()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsYoungRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsPinnedRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsLargeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsFreeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsGarbageRegion()),
                 region == nullptr ? 0u
                                   : static_cast<unsigned>(region->GetMarkBitmap() == nullptr &&
                                                          region->GetRegionAllocPtr() > region->GetRegionStart()));
            // HEADER_DUMP: first 64 bytes as hex + field decode + zap check.
            auto* bytes = reinterpret_cast<const uint8_t*>(object);
            char hex[64 * 2 + 16];
            size_t pos = 0;
            for (size_t i = 0; i < 64 && pos + 2 < sizeof(hex); ++i) {
                static const char* kHex = "0123456789abcdef";
                hex[pos++] = kHex[(bytes[i] >> 4) & 0xf];
                hex[pos++] = kHex[bytes[i] & 0xf];
            }
            hex[pos] = '\0';
            uint64_t w0 = 0;
            uint64_t w1 = 0;
            uint64_t w2 = 0;
            uint64_t w3 = 0;
            std::memcpy(&w0, bytes + 0, sizeof(w0));
            std::memcpy(&w1, bytes + 8, sizeof(w1));
            std::memcpy(&w2, bytes + 16, sizeof(w2));
            std::memcpy(&w3, bytes + 24, sizeof(w3));
            bool allZero = true;
            for (size_t i = 0; i < 64; ++i) {
                if (bytes[i] != 0) {
                    allZero = false;
                    break;
                }
            }
            bool isZap = HeapZap::IsZapWord(static_cast<uintptr_t>(w0));
            // tipBits: raw first 48 bits of header word (layout-dependent; not GetTypeInfo).
            uintptr_t tipBits = (static_cast<uintptr_t>(w0) & 0xffffffffffffULL);
            VLOG(REPORT,
                 "[GCV2][HEADER_DUMP] obj=%p hex64=%s w0=%#llx w1=%#llx w2=%#llx w3=%#llx "
                 "allZero=%u isZapWord=%u tipBits48=%#zx ZAP_WORD=%#llx "
                 "ZAP_VERDICT_%s",
                 object, hex, static_cast<unsigned long long>(w0), static_cast<unsigned long long>(w1),
                 static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3),
                 static_cast<unsigned>(allZero), static_cast<unsigned>(isZap), tipBits,
                 static_cast<unsigned long long>(HeapZap::ZAP_WORD),
                 isZap ? "是毒值_乙" : (allZero ? "非毒值_全零" : "非毒值_有内容"));
            VLOG(REPORT, "[GCV2][ROOT_ORIGIN] origin=%s obj=%p", src, object);
            // gcfwdfix: was this address inside a recent CompactRegion/ClearUnits zero range?
            char clearDetail[256];
            bool wasCleared = TraceClear::Lookup(reinterpret_cast<MAddress>(object), clearDetail, sizeof(clearDetail));
            VLOG(REPORT, "[GCV2][WAS_LIVE_BEFORE_CLEAR] hit=%u detail=%s obj=%p",
                 static_cast<unsigned>(wasCleared), clearDetail, object);
        }
        CHECK_DETAIL(false, "minor root/reference %p is not a valid object origin=%s", object, src);
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion() && !region->IsMarkedObject(object)) {
        workStack.push_back(object);
    }
}

// R3 markpar: STW-parallel young.mark_closure — sibling of ConcurrentMarkingWork
// (TracingCollector.cpp ConcurrentMarkingWork). Claim = MarkObject atomic bit;
// per-worker reachableVec/slots/weaks merged after pool barrier. Env:
// MRT_GCV2_MARKPAR_WORKERS / FORCE_SERIAL / INJECT_DISPEL.
// Port of fix/markpar@3f869baa onto setbitmap ledger (reachableVec + useBitmapLedger).
namespace {
// MarkStack::size() counts buffers (64 objs each), not objects. Major uses 16/8 for
// deep concurrent stacks; young LIFO DFS stays shallow ⇒ those thresholds never fire.
// Buffer-level 2/1 so steal engages on ~64–128 greys (markpar 3f869baa).
constexpr size_t kMarkparMaxWorkSize = 2;
constexpr size_t kMarkparMinWorkSize = 1;
} // namespace

struct YoungMarkingShared {
    WCollector* collector = nullptr;
    GCThreadPool* pool = nullptr;
    bool fullYoungScan = false;
    bool useBitmapLedger = true;
    bool recordSlots = false;
    std::vector<std::vector<BaseObject*>> objects;
    std::vector<std::vector<MAddress>> slots;
    std::vector<std::vector<MAddress>> weaks;
    std::vector<size_t> objectsMarked;
    std::atomic<size_t> nextWorkerId{ 0 };
};

class YoungMarkingWork : public HeapWork {
public:
    YoungMarkingWork(YoungMarkingShared& shared, TracingCollector::WorkStack&& stack, size_t workerSlot)
        : shared(shared), workStack(std::move(stack)), workerSlot(workerSlot)
    {}

    void TryForkTask()
    {
        if (shared.pool == nullptr) {
            return;
        }
        size_t size = workStack.size();
        if (size <= kMarkparMinWorkSize) {
            return;
        }
        bool doFork = false;
        size_t newSize = 0;
        if (size > kMarkparMaxWorkSize) {
            newSize = size >> 1;
            doFork = true;
        } else if (shared.pool->GetWaitingThreadNumber() > 0) {
            constexpr uint8_t shiftForEight = 3;
            newSize = size >> shiftForEight;
            doFork = true;
        }
        if (!doFork || newSize == 0) {
            return;
        }
        TracingCollector::WorkStackBuf* hSplit = workStack.split(newSize);
        size_t childSlot = shared.nextWorkerId.fetch_add(1, std::memory_order_relaxed);
        if (childSlot >= shared.objects.size()) {
            TracingCollector::WorkStack child(hSplit);
            while (!child.empty()) {
                workStack.push_back(child.back());
                child.pop_back();
            }
            return;
        }
        shared.pool->AddWork(new YoungMarkingWork(shared, TracingCollector::WorkStack(hSplit), childSlot));
    }

    void Execute(size_t) override
    {
        auto& localObjects = shared.objects[workerSlot];
        auto& localSlots = shared.slots[workerSlot];
        auto& localWeaks = shared.weaks[workerSlot];
        size_t nMarked = 0;
        // FYS non-young / legacy set: per-worker dedup (no shared set write under race).
        std::unordered_set<BaseObject*> localNonYoungSeen;
        WCollector* collector = shared.collector;
        const bool fullYoungScan = shared.fullYoungScan;
        const bool useBitmapLedger = shared.useBitmapLedger;
        const bool recordSlots = shared.recordSlots;

        auto pushTarget = [collector, fullYoungScan, this](RefField<>& field) {
            BaseObject* target = collector->ResolveMinorReference(field);
            if (fullYoungScan) {
                if (Heap::IsHeapAddress(target)) {
                    workStack.push_back(target);
                }
            } else {
                collector->PushYoungObject(target, workStack, "closure_edge");
            }
        };

        for (;;) {
            if (workStack.empty()) {
                break;
            }
            BaseObject* object = workStack.back();
            workStack.pop_back();
            if (!Heap::IsHeapAddress(object)) {
                continue;
            }
            if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
                BaseObject* host = Collector::TryRecoverInteriorBase(object);
                if (host != nullptr && host != object) {
                    workStack.push_back(host);
                }
                continue;
            }
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            const bool isYoung = region->IsYoungRegion();

            if (useBitmapLedger) {
                if (isYoung) {
                    bool wasMarked = collector->MarkObject(object);
                    if (wasMarked) {
                        continue;
                    }
                    ++nMarked;
                    CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
                    localObjects.push_back(object);
                } else if (!fullYoungScan) {
                    continue;
                } else {
                    if (!localNonYoungSeen.insert(object).second) {
                        continue;
                    }
                    CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
                    localObjects.push_back(object);
                }
            } else {
                if (isYoung) {
                    bool wasMarked = collector->MarkObject(object);
                    if (wasMarked) {
                        continue;
                    }
                    ++nMarked;
                    if (!localNonYoungSeen.insert(object).second) {
                        continue;
                    }
                } else if (!fullYoungScan) {
                    continue;
                } else if (!localNonYoungSeen.insert(object).second) {
                    continue;
                }
                CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
                localObjects.push_back(object);
            }

            if (!object->HasRefField()) {
                if (shared.pool != nullptr) {
                    TryForkTask();
                }
                continue;
            }
            if (UNLIKELY(object->IsWeakRef())) {
                HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
                localWeaks.push_back(reinterpret_cast<MAddress>(&referentField));
                BaseObject* referent = collector->ResolveMinorReference(referentField);
                if (!Heap::IsHeapAddress(referent)) {
                    if (shared.pool != nullptr) {
                        TryForkTask();
                    }
                    continue;
                }
                RegionInfo* referentRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
                if (referentRegion->IsYoungRegion()) {
                    WeakRefBuffer::Instance().Insert(object);
                }
                // weak referent double-scan: do not claim referent; N2 CAS converge.
                referent->ForEachRefField([&pushTarget](RefField<>& field) { pushTarget(field); });
                if (shared.pool != nullptr) {
                    TryForkTask();
                }
                continue;
            }
            object->ForEachRefField([&localSlots, &pushTarget, recordSlots](RefField<>& field) {
                if (recordSlots) {
                    localSlots.push_back(reinterpret_cast<MAddress>(&field));
                }
                pushTarget(field);
            });
            if (shared.pool != nullptr) {
                TryForkTask();
            }
        }
        shared.objectsMarked[workerSlot] += nMarked;
    }

private:
    YoungMarkingShared& shared;
    TracingCollector::WorkStack workStack;
    size_t workerSlot;
};

void WCollector::TraceYoungClosureSerial(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                         std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                         MinorSlotSet& weakSlots, bool useBitmapLedger)
{
    // setbitmap O1③: useBitmapLedger=true → claim young via MarkObject (region mark bitmap)
    // + collect into reachableVec; non-young under FYS still uses reachableObjects set.
    // FYS=0: skip reachableSlots inserts (lookups never fire; T1 measured pure write cost).
    const bool recordSlots = fullYoungScan; // only FYS path looks up reachableSlots
    auto pushTarget = [this, fullYoungScan, &workStack](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(target)) {
                workStack.push_back(target);
            }
        } else {
            PushYoungObject(target, workStack, "closure_edge");
        }
    };
    while (!workStack.empty()) {
        BaseObject* object = workStack.back();
        workStack.pop_back();
        if (!Heap::IsHeapAddress(object)) {
            continue;
        }
        // markfloor / introot: RawArray+8 interiors pass IsValidObject (tip=length≠null)
        // then HasRefField/GetSize SEGV. Recover host; skip interior itself.
        if (!Collector::PlausibleManagedObjectGate("TraceYoungClosure", object)) {
            BaseObject* host = Collector::TryRecoverInteriorBase(object);
            if (host != nullptr && host != object) {
                workStack.push_back(host);
            }
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        const bool isYoung = region->IsYoungRegion();

        if (useBitmapLedger) {
            if (isYoung) {
                // MarkObject returns true if already marked → skip re-visit.
                if (MarkObject(object)) {
                    continue;
                }
                reachableVec.push_back(object);
            } else {
                // FYS path may visit non-young holders; claim via set (no mark bitmap on old).
                if (!fullYoungScan) {
                    continue;
                }
                if (!LedgerInsert(reachableObjects, object, g_minorLedgerCost.objInsN, g_minorLedgerCost.objInsNew,
                                  g_minorLedgerCost.objInsNs)) {
                    continue;
                }
                reachableVec.push_back(object);
            }
        } else {
            if (!LedgerInsert(reachableObjects, object, g_minorLedgerCost.objInsN, g_minorLedgerCost.objInsNew,
                              g_minorLedgerCost.objInsNs)) {
                continue;
            }
            if (isYoung) {
                (void)MarkObject(object);
            } else if (!fullYoungScan) {
                continue;
            }
            reachableVec.push_back(object);
        }

        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField = HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            (void)LedgerInsert(weakSlots, reinterpret_cast<MAddress>(&referentField), g_minorLedgerCost.weakInsN,
                               g_minorLedgerCost.weakInsNew, g_minorLedgerCost.weakInsNs);
            BaseObject* referent = ResolveMinorReference(referentField);
            if (!Heap::IsHeapAddress(referent)) {
                continue;
            }
            RegionInfo* referentRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
            if (referentRegion->IsYoungRegion()) {
                WeakRefBuffer::Instance().Insert(object);
            }
            referent->ForEachRefField([&pushTarget](RefField<>& field) { pushTarget(field); });
            continue;
        }
        object->ForEachRefField([&reachableSlots, &pushTarget, recordSlots](RefField<>& field) {
            if (recordSlots) {
                (void)LedgerInsert(reachableSlots, reinterpret_cast<MAddress>(&field), g_minorLedgerCost.slotInsN,
                                   g_minorLedgerCost.slotInsNew, g_minorLedgerCost.slotInsNs);
            }
            pushTarget(field);
        });
    }
}

void WCollector::TraceYoungClosureParallel(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                           std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                           MinorSlotSet& weakSlots, bool useBitmapLedger, GCThreadPool* threadPool)
{
    // T-D ③: dispel frozen across parallel mark window (same as R2 reffix).
    const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
    {
        const char* inject = std::getenv("MRT_GCV2_MARKPAR_INJECT_DISPEL");
        if (inject != nullptr && std::strcmp(inject, "1") == 0) {
            RegionInfo::InjectDispelCountForTest();
            VLOG(REPORT, "[GCV2][markpar] inject_dispel=1 (positive control)");
        }
    }

    const int32_t helperNum = threadPool->GetMaxThreadNum();
    int32_t poolCap = helperNum + 1;
    int32_t workers = poolCap;
    {
        const char* wEnv = std::getenv("MRT_GCV2_MARKPAR_WORKERS");
        if (wEnv != nullptr && wEnv[0] != '\0') {
            int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
            if (want >= 1 && want < workers) {
                workers = want;
            }
        }
    }
    if (workers < 1) {
        workers = 1;
    }
    // workers=1 apparatus: main only, no pool Start (markpar 0cd9df7c).
    if (workers == 1) {
        TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                useBitmapLedger);
        VLOG(REPORT,
             "[GCV2][markpar][parallel] workers_active=1 workers_scheduled=1 objects_marked=[%zu] "
             "reachable_n=%zu parallel=0",
             reachableVec.size(), reachableVec.size());
        return;
    }

    const size_t slotBudget = static_cast<size_t>(workers) * 8 + 16;
    YoungMarkingShared shared;
    shared.collector = this;
    shared.pool = threadPool;
    shared.fullYoungScan = fullYoungScan;
    shared.useBitmapLedger = useBitmapLedger;
    shared.recordSlots = fullYoungScan;
    shared.objects.resize(slotBudget);
    shared.slots.resize(slotBudget);
    shared.weaks.resize(slotBudget);
    shared.objectsMarked.assign(slotBudget, 0);
    shared.nextWorkerId.store(1, std::memory_order_relaxed);

    const int32_t prevActive = threadPool->GetMaxActiveThreadNum();
    const int32_t wantActive = workers - 1;
    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(wantActive);
    }

    // Seed: peel root buffers to helpers first, then Start + main + WaitFinish.
    size_t slot = 1;
    while (workStack.size() > 1 && slot < static_cast<size_t>(workers)) {
        TracingCollector::WorkStackBuf* hSplit = workStack.split(1);
        if (hSplit == nullptr) {
            break;
        }
        threadPool->AddWork(new YoungMarkingWork(shared, TracingCollector::WorkStack(hSplit), slot));
        shared.nextWorkerId.store(slot + 1, std::memory_order_relaxed);
        ++slot;
    }
    threadPool->Start();
    YoungMarkingWork mainTask(shared, std::move(workStack), 0);
    mainTask.Execute(0);
    threadPool->WaitFinish();

    if (wantActive != prevActive) {
        threadPool->SetMaxActiveThreadNum(prevActive);
    }

    const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
    CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                 "T-D ghost dispel during parallel mark_closure window entry=%zu exit=%zu", dispelAtEntry,
                 dispelAtExit);

    // Merge per-worker ledgers → global reachableVec / sets (downstream ⑦ consumes them).
    size_t active = 0;
    std::string markedStr;
    for (size_t i = 0; i < shared.objects.size(); ++i) {
        if (shared.objects[i].empty() && shared.slots[i].empty() && shared.weaks[i].empty() &&
            shared.objectsMarked[i] == 0) {
            continue;
        }
        if (shared.objectsMarked[i] != 0) {
            ++active;
        }
        if (!markedStr.empty()) {
            markedStr += ',';
        }
        markedStr += std::to_string(shared.objectsMarked[i]);
        for (BaseObject* obj : shared.objects[i]) {
            if (!useBitmapLedger) {
                reachableObjects.insert(obj);
            } else {
                RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region != nullptr && !region->IsYoungRegion() && fullYoungScan) {
                    reachableObjects.insert(obj);
                }
            }
            reachableVec.push_back(obj);
        }
        for (MAddress s : shared.slots[i]) {
            reachableSlots.insert(s);
        }
        for (MAddress s : shared.weaks[i]) {
            weakSlots.insert(s);
        }
    }
    if (markedStr.empty()) {
        markedStr = "0";
    }

    VLOG(REPORT,
         "[GCV2][markpar][parallel] workers_active=%zu workers_scheduled=%d objects_marked=[%s] "
         "reachable_n=%zu parallel=1",
         active, workers, markedStr.c_str(), reachableVec.size());
}

void WCollector::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                   std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                   MinorSlotSet& weakSlots, bool useBitmapLedger)
{
    if (workStack.empty()) {
        return;
    }
    GCThreadPool* threadPool = GetThreadPool();
    static const bool forceSerialEnv = []() {
        const char* value = std::getenv("MRT_GCV2_MARKPAR_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool useParallel = threadPool != nullptr && !forceSerialEnv;
    if (!useParallel) {
        VLOG(REPORT, "[GCV2][markpar][parallel] fallback=serial pool_unavailable");
        TraceYoungClosureSerial(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                useBitmapLedger);
        VLOG(REPORT,
             "[GCV2][markpar][parallel] workers_active=1 workers_scheduled=1 objects_marked=[%zu] "
             "reachable_n=%zu parallel=0",
             reachableVec.size(), reachableVec.size());
        return;
    }
    TraceYoungClosureParallel(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                              useBitmapLedger, threadPool);
}

// youngconc: SATB termination for concurrent young mark — same loop shape as
// TracingCollector::MarkSatbBuffer, but feeds TraceYoungClosure (young claim + FYS).
// Mutators run under TraceBarrier (InstallBarrier TRACE); final CLEAR_SATB is STW.
bool WCollector::MarkYoungSatbBuffer(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                     std::vector<BaseObject*>& reachableVec, MinorSlotSet& reachableSlots,
                                     MinorSlotSet& weakSlots, bool useBitmapLedger)
{
    MRT_PHASE_TIMER("young.mark_satb");
    workStack.clear();
    constexpr uint64_t maxIterationTime = 120ULL * 1000 * 1000 * 1000;
    constexpr uint64_t maxIterationLoopNum = 1000;
    auto visitSatbObj = [this, fullYoungScan, &workStack]() {
        WorkStack remarkStack;
        SatbBuffer::Instance().GetRetiredObjects(remarkStack);
        while (!remarkStack.empty()) {
            BaseObject* obj = remarkStack.back();
            remarkStack.pop_back();
            if (!Heap::IsHeapAddress(obj)) {
                continue;
            }
            if (fullYoungScan) {
                workStack.push_back(obj);
            } else {
                PushYoungObject(obj, workStack, "young_satb");
            }
        }
    };
    visitSatbObj();
    uint64_t iterationCnt = 0;
    uint64_t iterationStartTime = TimeUtil::NanoSeconds();
    do {
        if (++iterationCnt > maxIterationLoopNum && (TimeUtil::NanoSeconds() - iterationStartTime) > maxIterationTime) {
            ScopedStopTheWorld stw("MarkYoungSatbBuffer timeout", true, GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
            VLOG(REPORT, "[GCV2][youngconc] MarkYoungSatbBuffer timeout STW drain");
            visitSatbObj();
            TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                              useBitmapLedger);
            return workStack.empty();
        }
        if (!workStack.empty()) {
            TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                              useBitmapLedger);
        }
        visitSatbObj();
        if (workStack.empty()) {
            TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
            visitSatbObj();
        }
    } while (!workStack.empty());
    return true;
}

void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     bool fullYoungScan, MinorSlotSet* consumedOut, DiffPathRemsetStats* statsOut)
{
    // HotSpot G1RemSet scrub analogue. ORDER matters (STEER2 / defect⑤):
    //   1) region-level holder_dead (free/garbage region only — not object liveness)
    //   2) pre-check target safety BEFORE ResolveMinorReference
    //      (old-tag with no to-version + invalid from must not reach FindLatestVersion/F5)
    //   3) ResolveMinorReference (soft-resolve; never calls FindLatestVersion)
    //   4) post-resolve null / bad_target drops
    // Does not relax IsValidObject / FindLatestVersion CHECK_DETAIL.
    static std::atomic<size_t> g_remsetScrubLogged{ 0 };
    size_t scrubbedStale = 0;
    size_t scrubbedDeadHolder = 0;
    size_t scrubbedBadTarget = 0;
    size_t scrubbedStaleOldTag = 0;
    static const bool retainedProbe = []() {
        const char* value = std::getenv("MRT_GCV2_RETLIVE_PROBE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    size_t originFound = 0;
    size_t originBoundsValid = 0;
    size_t retainedNever = 0;
    size_t retainedValid = 0;
    size_t retainedEmpty = 0;
    size_t retainedStale = 0;
    size_t retainedKeep = 0;
    size_t retainedDrop = 0;
    size_t safeEmptyDrop = 0;
    size_t directDeadDrop = 0;
    size_t filterCorrect = 0;
    size_t filterIncorrect = 0;
    // The precise bitmap intentionally stores only field-slot identity. Recover an
    // object origin only for regions whose retained snapshot is consumable (or when
    // the default-off probe requests visibility), and keep that adapter local to this
    // minor collection rather than adding a second persistent remset index.
    std::unordered_map<MAddress, BaseObject*> rememberedOrigins;
    std::unordered_set<RegionInfo*> originRegions;
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        RegionInfo::RetainedLiveInfoState retainedState = region->GetRetainedLiveInfoState();
        if (retainedProbe || (retainedState != RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED &&
                             region->IsRetainedSnapshotValid())) {
            originRegions.insert(region);
        }
    }
    for (RegionInfo* region : originRegions) {
        region->VisitAllObjects([&rememberedSlots, &rememberedOrigins](BaseObject* holder) {
            if (holder == nullptr || !holder->HasRefField()) {
                return;
            }
            holder->ForEachRefField([holder, &rememberedSlots, &rememberedOrigins](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                // rememberedSlots is the remset drain set (not the mark ledger); leave untimed.
                if (rememberedSlots.count(slot) != 0) {
                    rememberedOrigins[slot] = holder;
                }
            });
        });
    }
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            if (statsOut != nullptr) {
                ++statsOut->skippedNotHeap;
            }
            continue;
        }
        if (LedgerCount(weakSlots, slot, g_minorLedgerCost.weakLookN, g_minorLedgerCost.weakLookNs) != 0) {
            if (statsOut != nullptr) {
                ++statsOut->skippedWeak;
            }
            continue;
        }
        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(slot);
        if (holderRegion == nullptr || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion()) {
            ++scrubbedDeadHolder;
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx reason=holder_dead region=%p free=%u garbage=%u",
                     static_cast<size_t>(slot), holderRegion,
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsGarbageRegion()));
            }
            continue;
        }

        bool keepByRetainedSnapshot = true;
        auto originIt = rememberedOrigins.find(slot);
        if (originIt != rememberedOrigins.end() && originIt->second != nullptr &&
            Heap::IsHeapAddress(originIt->second)) {
            BaseObject* holder = originIt->second;
            RegionInfo* originRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (originRegion == holderRegion) {
                if (retainedProbe) {
                    ++originFound;
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    size_t holderSize = RegionSpace::GetAllocSize(*holder);
                    if (slot >= holderAddress && slot < holderAddress + holderSize) {
                        ++originBoundsValid;
                    }
                }
                RegionInfo::RetainedLiveInfoState retainedState = holderRegion->GetRetainedLiveInfoState();
                if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
                    if (retainedProbe) {
                        ++retainedNever;
                    }
                } else if (!holderRegion->IsRetainedSnapshotValid()) {
                    if (retainedProbe) {
                        ++retainedStale;
                    }
                } else {
                    MAddress coveredUpTo = holderRegion->GetRetainedLiveInfoCoveredUpTo();
                    CHECK(coveredUpTo >= holderRegion->GetRegionStart() &&
                          coveredUpTo <= holderRegion->GetRegionAllocPtr());
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                        if (retainedProbe) {
                            ++retainedEmpty;
                        }
                    } else {
                        if (retainedProbe) {
                            ++retainedValid;
                        }
                    }
                    if (holderAddress < coveredUpTo) {
                        if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                            keepByRetainedSnapshot = false;
                            if (retainedProbe) {
                                ++safeEmptyDrop;
                            }
                        } else if (holderRegion->IsLargeRegion()) {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            keepByRetainedSnapshot = retainedLiveInfo != nullptr
                                ? retainedLiveInfo->IsSurvivedObject(0)
                                : holderRegion->IsSurvivedObject(0);
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
                        } else {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            CHECK(retainedLiveInfo != nullptr);
                            size_t holderOffset = holderRegion->GetAddressOffset(holderAddress);
                            keepByRetainedSnapshot = retainedLiveInfo->IsSurvivedObject(holderOffset);
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
                        }
                    }
                }
            }
        }
        if (retainedProbe) {
            if (keepByRetainedSnapshot) {
                ++retainedKeep;
            } else {
                ++retainedDrop;
            }
        }
        if (fullYoungScan) {
            bool oracleKeep =
                LedgerCount(reachableSlots, slot, g_minorLedgerCost.slotLookN, g_minorLedgerCost.slotLookNs) != 0;
            if (retainedProbe) {
                if (keepByRetainedSnapshot == oracleKeep) {
                    ++filterCorrect;
                } else {
                    ++filterIncorrect;
                }
            }
            if (!oracleKeep) {
                if (statsOut != nullptr) {
                    ++statsOut->skippedFysFilter;
                }
                continue;
            }
        } else if (!keepByRetainedSnapshot) {
            ++scrubbedDeadHolder;
            continue;
        }

        HeapSlot<>* field = &HeapSlotAt<>(slot);
        uint64_t rawSlot = 0;
        std::memcpy(&rawSlot, field, sizeof(rawSlot));
        RefField<> peek(*field);
        BaseObject* rawTarget = to_object(peek.GetTargetObject());
        // Pre-check (before resolve): one-gen-stale old-tag whose from has no to-version
        // and is not a live object — drop without FindLatestVersion (F5 fail-closed stays).
        if (IsOldPointer(peek)) {
            BaseObject* to = FindToVersion(rawTarget);
            bool fromLive = false;
            if (to == nullptr && Heap::IsHeapAddress(rawTarget)) {
                RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(rawTarget));
                fromLive = fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
                           rawTarget->IsValidObject();
            }
            // Non-heap target: FindToVersion null + fromLive false is expected (not dead).
            // Do not CAS-null — slot may be RO; drop remset edge only via fall-through scrub.
            if (to == nullptr && !fromLive &&
                (rawTarget == nullptr || Heap::IsHeapAddress(rawTarget))) {
                ++scrubbedStaleOldTag;
                // N2: CAS null install (same slot may race with ResolveMinorReference under FYS=1).
                NoteNullslotWrite("remset_stale_oldtag", nullptr, field, rawTarget, to, &g_nullslotRemset);
                (void)CasInstallResolvedTarget(*field, raw(peek.GetFieldValue()), RefField<>(nullptr));
                size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
                if (n < 16) {
                    VLOG(REPORT,
                         "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=stale_oldtag "
                         "(no to-version; from invalid/reclaimed — pre-resolve)",
                         static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), rawTarget);
                }
                continue;
            }
        }

        BaseObject* target = ResolveMinorReference(*field);
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            ++scrubbedStale;
            continue;
        }
        if (!target->IsValidObject()) {
            ++scrubbedBadTarget;
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=bad_target "
                     "holderYoung=%u holderFree=%u targetYoung=%u targetFree=%u targetGarbage=%u "
                     "targetNeverExamined=%u (H1: stale remset after reclaim)",
                     static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), target,
                     static_cast<unsigned>(holderRegion->IsYoungRegion()),
                     static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsYoungRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsGarbageRegion()),
                     targetRegion == nullptr
                         ? 0u
                         : static_cast<unsigned>(targetRegion->GetMarkBitmap() == nullptr &&
                                                 targetRegion->GetRegionAllocPtr() > targetRegion->GetRegionStart()));
            }
            continue;
        }

        PushYoungObject(target, workStack, "remset");
        if (consumedOut != nullptr) {
            consumedOut->insert(slot);
        }
        if (statsOut != nullptr) {
            ++statsOut->consumed;
        }
    }
    if (scrubbedStale != 0 || scrubbedDeadHolder != 0 || scrubbedBadTarget != 0 || scrubbedStaleOldTag != 0) {
        VLOG(REPORT,
             "[GCV2][remset-filter] summary staleTarget=%zu deadHolderRegion=%zu badTarget=%zu "
             "staleOldTag=%zu recorded=%zu "
             "(DEAD_HOLDER_DROPPED≈deadHolderRegion+staleOldTag; region-level holder_dead ≠ object-dead)",
             scrubbedStale, scrubbedDeadHolder, scrubbedBadTarget, scrubbedStaleOldTag, rememberedSlots.size());
    }
    if (retainedProbe) {
        VLOG(REPORT,
             "[RETLIVE][summary] slots=%zu originFound=%zu originBoundsValid=%zu never=%zu valid=%zu empty=%zu "
             "stale=%zu keep=%zu drop=%zu safeEmpty=%zu directDead=%zu oracleCorrect=%zu oracleIncorrect=%zu "
             "fullYoungScan=%u",
             rememberedSlots.size(), originFound, originBoundsValid, retainedNever, retainedValid, retainedEmpty,
             retainedStale, retainedKeep, retainedDrop, safeEmptyDrop, directDeadDrop, filterCorrect,
             filterIncorrect, static_cast<unsigned>(fullYoungScan));
    }
}

bool WCollector::FixMinorEvacuatedSlot(RefField<>& field) const
{
    // N1: major-style CAS tolerate (TryUpdateRefFieldImpl family). Under multi-worker
    // fix, CAS fail is normal (peer already updated) — abort assertion was serial-only.
    RefField<> oldField(field);
    BaseObject* target = ResolveMinorReference(field);
    // Static / RO slots may hold non-heap objects (never evacuated). Colouring them
    // changes the bit pattern so equal-skip misses, then CAS faults on RELRO.
    // Same heap gate as ForwardUpdateRawRef / FindToVersion.
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    // interiorsrc2 / introot: value may be RawArray+8 (derived interior). Relocate via host;
    // write plain only. Storage is still HeapSlot (fields/remset) — DerivedSlot cannot CAS
    // into it; CasInstallInteriorPlain names the (host,offset) provenance (derivedtype).
    // ScopedPlainWriter tags DerivedLegal column, not K1 HeapSlot plain.
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot", target)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(target);
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), host);
            BaseObject* toHost = const_cast<WCollector*>(this)->ForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                size_t offset = static_cast<size_t>(reinterpret_cast<uintptr_t>(target) -
                                                    reinterpret_cast<uintptr_t>(host));
                MAddress oldVal = raw(oldField.GetFieldValue());
                MAddress plainVal = reinterpret_cast<MAddress>(toHost) + offset;
                if (oldVal != plainVal) {
                    ScopedPlainWriter tag(PlainWriterSite::FixMinorInterior);
                    (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), toHost, offset);
                }
                return true;
            }
        }
        // Gate rejected; host unknown or not forwarded — still plain interior (03fc21ed).
        MAddress oldVal = raw(oldField.GetFieldValue());
        MAddress plainVal = reinterpret_cast<MAddress>(target);
        if (oldVal != plainVal) {
            ScopedPlainWriter tag(PlainWriterSite::FixMinorInterior);
            (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), target);
        }
        return false;
    }
    BaseObject* current = target;
    if (IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        // installdomain: route-domain grant before ForwardObject → GetRoute.
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    // ForwardObject may return the same interior if gated; re-check before colouring.
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.postfwd", current)) {
        MAddress oldVal = raw(field.GetFieldValue());
        MAddress plainVal = reinterpret_cast<MAddress>(current);
        if (oldVal != plainVal) {
            ScopedPlainWriter tag(PlainWriterSite::FixMinorInterior);
            (void)CasInstallInteriorPlain(field, to_zpointer(oldVal), current);
        }
        return false;
    }
    // plainroots: stack/reg root slots → plain current; heap remset/fields → Phase C colour.
    // Plain on heap was the trust-state install that AssertColouredWriteIfEnabled fires on.
    RefField<> newField = RootSlotWriteback(current, &field);
    MAddress oldVal = raw(oldField.GetFieldValue());
    MAddress newVal = raw(newField.GetFieldValue());
    if (oldVal == newVal) {
        return false;
    }
    // Re-read after resolve (resolve may have CAS-installed plain already).
    oldVal = raw(field.GetFieldValue());
    if (oldVal == newVal) {
        return false;
    }
    if (field.CompareExchange(to_zpointer(oldVal), to_zpointer(newVal))) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // CAS fail: accept if current == desired or already a plain/newer install (major style).
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    MAddress cur = raw(field.GetFieldValue());
    if (cur == newVal) {
        return true;
    }
    // Peer may have installed same logical target via ResolveMinorReference first
    // (old tagged → plain) then another worker forwarded; either is a valid fix.
    return true;
}

bool WCollector::FixMinorEvacuatedSlot(RootSlot& root) const
{
    MAddress oldValue = raw(root.LoadPlain());
    BaseObject* target = ResolveMinorReference(root);
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return false;
    }
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot", target)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(target);
        if (host != nullptr && IsGhostFromObject(host) && !IsUnmovableFromObject(host)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), host);
            BaseObject* toHost = const_cast<WCollector*>(this)->ForwardObject(host);
            if (toHost != nullptr && toHost != host) {
                BaseObject* toInterior = reinterpret_cast<BaseObject*>(
                    reinterpret_cast<uintptr_t>(toHost) +
                    (reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(host)));
                HealRoot(root, from_object(toInterior));
                return true;
            }
        }
        HealRoot(root, from_object(target));
        return false;
    }
    BaseObject* current = target;
    if (IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        EnsureRouteDomainMembership(const_cast<WCollector*>(this), target);
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    if (!Collector::PlausibleManagedObjectGate("FixMinorEvacuatedSlot.postfwd", current)) {
        HealRoot(root, from_object(current));
        return false;
    }
    MAddress newValue = reinterpret_cast<MAddress>(current);
    if (oldValue == newValue && raw(root.LoadPlain()) == newValue) {
        return false;
    }
    HealRoot(root, from_object(current));
    return true;
}

void WCollector::FixMinorRootSlots()
{
    RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
    // Must match VisitMinorRootSlots: mutator_stack was enumerated at mark time but
    // previously omitted here (defect④ / stdbuildflag). After EvacuateYoungRegions,
    // stack slots still holding from-copies become the next full's F5 input.
    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    Heap::GetHeap().VisitStaticRoots(rawRootVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
}

// fixinput: FixMinorObjectSlots reader-side accounting (default on, cheap atomics).
// Reject arm must not silent-drop a real interior edge: recover host when Plausible.
// tip-in-heap / non-object: no legitimate field edges — account + sample, no invent.
namespace {
std::atomic<size_t> g_fixinputReject{ 0 };
std::atomic<size_t> g_fixinputRecover{ 0 };
std::atomic<size_t> g_fixinputUnrecoverable{ 0 };
} // namespace

void WCollector::FixMinorObjectSlots(BaseObject* object)
{
    // secondclass ②: belt-and-braces — refuse null tip before HasRefField.
    if (object == nullptr || !object->IsValidObject()) {
        return;
    }
    // fixinput / nilclass 丙: mark side already uses PlausibleManagedObjectGate
    // (PushYoungObject / TraceYoungClosure); Fix only had IsValidObject (tip≠null).
    // Coloured heap ref as tip (tip-in-heap) still passes IsValidObject → SEGV_nil in
    // ForEachBitmapWord. Reuse gate semantics at the consumer; do not relax the gate.
    if (!Collector::PlausibleManagedObjectGate("FixMinorObjectSlots", object)) {
        size_t n = g_fixinputReject.fetch_add(1, std::memory_order_relaxed) + 1;
        BaseObject* host = Collector::TryRecoverInteriorBase(object);
        // Only rescan when host itself is a real managed object (classic RawArray+8).
        // tip-in-heap residuals must not invent a false host via ClassifyInteriorOffset.
        if (host != nullptr && host != object &&
            Collector::PlausibleManagedObjectGate("FixMinorObjectSlots.host", host)) {
            g_fixinputRecover.fetch_add(1, std::memory_order_relaxed);
            FixMinorObjectSlots(host);
            return;
        }
        g_fixinputUnrecoverable.fetch_add(1, std::memory_order_relaxed);
        // Edge disposition: not a managed object header — no legitimate field edges.
        if (n <= 16) {
            LOG(RTLOG_ERROR,
                "[GCV2][fixinput] reject FixMinorObjectSlots obj=%p tip=%p n=%zu "
                "reason=non-object-no-host (edge: no field walk; host unknown)",
                object, object->GetTypeInfo(), n);
        }
        return;
    }
    if (!object->HasRefField()) {
        return;
    }
    object->ForEachRefField([this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); });
}

// R2: parallel ⑦ young.ref_fix — index-shard reachableObjects + remset slots;
// root families = 6 family-level tasks (static not split). Template = A2 stwpar2.
// Env: MRT_GCV2_REFFIX_WORKERS, MRT_GCV2_REFFIX_FORCE_SERIAL.
void WCollector::FixMinorRootSlotsParallel(GCThreadPool* threadPool)
{
    // 5 root families as separate tasks (static kept whole — mutex+dedup set).
    // Order matches serial FixMinorRootSlots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
        MutatorManager::Instance().VisitAllMutators(
            [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
        Heap::GetHeap().VisitStaticRoots(rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
        collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) { (void)FixMinorEvacuatedSlot(root); };
        Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
    }));
}

void WCollector::EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec,
                                       const MinorSlotSet& rememberedSlots)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    auto postEvacPoint = [this](const char* point, bool runHeap = true) {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac == nullptr || std::strcmp(postEvac, "1") != 0) {
            return;
        }
        // Breadcrumb first (survives if VerifyHeap SEGV); force=true skips VERIFY_HEAP env.
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=%s run=%zu", point, minorTotalRuns + 1);
        if (runHeap) {
            VerifyHeapObjects(point, true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=%s run=%zu", point, minorTotalRuns + 1);
        }
    };
    // fixinput: grant route-domain before holder Forward (same as FixMinorEvacuatedSlot).
    // Do not rewrite holders or soft-skip Forward here — gold regressed when from_fallback
    // left unfixed from-faces. Bad to-tip is refused at FixMinorObjectSlots (reader gate).
    auto currentObject = [this](BaseObject* object) {
        if (IsGhostFromObject(object) && !IsUnmovableFromObject(object)) {
            EnsureRouteDomainMembership(const_cast<WCollector*>(this), object);
            return ForwardObject(object);
        }
        return object;
    };

    // reachableVec already materialised by TraceYoungClosure (setbitmap O1③) —
    // no unordered_set → vector copy. remset still from set (slot identity).
    std::vector<MAddress> remsetVec(rememberedSlots.begin(), rememberedSlots.end());

    auto fixHeapSlice = [this, &reachableVec, &remsetVec, &currentObject](size_t beginObj, size_t endObj,
                                                                           size_t beginSlot, size_t endSlot,
                                                                           size_t& objectsTaken) {
        for (size_t i = beginObj; i < endObj; ++i) {
            FixMinorObjectSlots(currentObject(reachableVec[i]));
            ++objectsTaken;
        }
        for (size_t i = beginSlot; i < endSlot; ++i) {
            MAddress slot = remsetVec[i];
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot));
            }
        }
    };

    auto fixForwardedReferencesSerial = [this, &reachableVec, &remsetVec, &currentObject]() {
        FixMinorRootSlots();
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        for (BaseObject* object : reachableVec) {
            FixMinorObjectSlots(currentObject(object));
        }
        for (MAddress slot : remsetVec) {
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(HeapSlotAt<>(slot));
            }
        }
    };

    auto fixForwardedReferencesParallel = [this, &reachableVec, &remsetVec, &fixHeapSlice](GCThreadPool* pool) {
        // T-D ③: dispel must stay frozen across the parallel window.
        const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
        // Positive control: MRT_GCV2_REFFIX_INJECT_DISPEL=1 forces a synthetic bump so
        // the assertion path is proven to fire (not a silent always-pass).
        {
            const char* inject = std::getenv("MRT_GCV2_REFFIX_INJECT_DISPEL");
            if (inject != nullptr && std::strcmp(inject, "1") == 0) {
                RegionInfo::InjectDispelCountForTest();
                VLOG(REPORT, "[GCV2][reffix] inject_dispel=1 (positive control)");
            }
        }

        const size_t nObj = reachableVec.size();
        const size_t nSlot = remsetVec.size();
        const int32_t helperNum = pool->GetMaxThreadNum();
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* wEnv = std::getenv("MRT_GCV2_REFFIX_WORKERS");
            if (wEnv != nullptr && wEnv[0] != '\0') {
                int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
                if (want >= 1 && want < heapWorkers) {
                    heapWorkers = want;
                }
            }
        }
        // At least 1 heap worker; root families = 5 additional tasks.
        if (heapWorkers < 1) {
            heapWorkers = 1;
        }
        std::vector<size_t> objectsTaken(static_cast<size_t>(heapWorkers), 0);
        std::atomic<size_t> objCursor{ 0 };
        std::atomic<size_t> slotCursor{ 0 };
        // Chunk size: aim ~heapWorkers*4 grabs for load balance; min 64 objects.
        const size_t objChunk = std::max<size_t>(64, (nObj + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                        (static_cast<size_t>(heapWorkers) * 4 + 1));
        const size_t slotChunk = std::max<size_t>(64, (nSlot + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                         (static_cast<size_t>(heapWorkers) * 4 + 1));

        FixMinorRootSlotsParallel(pool);
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));

        for (int32_t w = 0; w < heapWorkers; ++w) {
            size_t* taken = &objectsTaken[static_cast<size_t>(w)];
            pool->AddWork(new (std::nothrow) LambdaWork(
                [fixHeapSlice, &objCursor, &slotCursor, nObj, nSlot, objChunk, slotChunk, taken](size_t) {
                    for (;;) {
                        size_t o0 = nObj;
                        size_t o1 = nObj;
                        size_t s0 = nSlot;
                        size_t s1 = nSlot;
                        bool got = false;
                        if (objCursor.load(std::memory_order_relaxed) < nObj) {
                            o0 = objCursor.fetch_add(objChunk, std::memory_order_relaxed);
                            if (o0 < nObj) {
                                o1 = std::min(o0 + objChunk, nObj);
                                got = true;
                            } else {
                                o0 = o1 = nObj;
                            }
                        }
                        if (slotCursor.load(std::memory_order_relaxed) < nSlot) {
                            s0 = slotCursor.fetch_add(slotChunk, std::memory_order_relaxed);
                            if (s0 < nSlot) {
                                s1 = std::min(s0 + slotChunk, nSlot);
                                got = true;
                            } else {
                                s0 = s1 = nSlot;
                            }
                        }
                        if (!got) {
                            break;
                        }
                        fixHeapSlice(o0, o1, s0, s1, *taken);
                    }
                }));
        }

        pool->Start();
        pool->WaitFinish();

        const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
        CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                     "T-D ghost dispel during parallel ref_fix window entry=%zu exit=%zu "
                     "(plain InGhostFromRegion read assumes phase isolation)",
                     dispelAtEntry, dispelAtExit);

        size_t active = 0;
        std::string takenStr;
        for (size_t i = 0; i < objectsTaken.size(); ++i) {
            if (objectsTaken[i] != 0) {
                ++active;
            }
            if (i != 0) {
                takenStr += ',';
            }
            takenStr += std::to_string(objectsTaken[i]);
        }
        VLOG(REPORT,
             "[GCV2][reffix][parallel] workers_active=%zu workers_scheduled=%d objects_taken=[%s] "
             "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=1",
             active, heapWorkers, takenStr.c_str(), nObj, nSlot,
             g_minorRefCasOk.load(std::memory_order_relaxed),
             g_minorRefCasFail.load(std::memory_order_relaxed));
    };

    // Earliest post-mark checkpoint: still before any fix/forward mutates refs.
    postEvacPoint("evac-enter", true);

    {
        // minortime: ⑦ ref fix (preforward roots + fixForwardedReferences)
        MRT_PHASE_TIMER("young.ref_fix");
        TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);

        GCThreadPool* threadPool = GetThreadPool();
        static const bool forceSerialEnv = []() {
            const char* value = std::getenv("MRT_GCV2_REFFIX_FORCE_SERIAL");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        const bool forceSerial = forceSerialEnv;
        const bool useParallel = threadPool != nullptr && !forceSerial;

        // pass1 root fix (before PrepareForwardTable) — serial sandwich stays;
        // only the post-map fixForwardedReferences body is parallelized (⑦ bulk).
        // pass1 is load-bearing for previous-gen residual (MINOR_CONCURRENCY §七 T-A).
        FixMinorRootSlots();
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        postEvacPoint("post-preforward-roots", false); // breadcrumb only — avoid SEGV before fix body

        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
        fwdTable.PrepareForwardTable();
        TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
        postEvacPoint("pre-fix-forwarded", false);

        // installdomain: serial pre-grant while every from region is still FORWARDABLE.
        // Must finish before any Fix/Forward can RouteRegion→ROUTED (else liveByteCount
        // geometry freezes without the late survivor). Walk remset + reachable + roots
        // (same surface as fixForwardedReferences) and also grant the holder itself.
        {
            auto ensureObj = [this](BaseObject* t) {
                EnsureRouteDomainMembership(const_cast<WCollector*>(this), t);
            };
            auto ensureField = [&ensureObj](RefField<>& field) {
                RefField<> value(field);
                ensureObj(to_object(value.GetTargetObject()));
            };
            // Holders first (currentObject will ForwardObject them).
            for (BaseObject* object : reachableVec) {
                ensureObj(object);
            }
            for (MAddress slot : remsetVec) {
                if (Heap::IsHeapAddress(slot)) {
                    ensureField(HeapSlotAt<>(slot));
                }
            }
            for (BaseObject* object : reachableVec) {
                if (object == nullptr || !Heap::IsHeapAddress(object)) {
                    continue;
                }
                if (!Collector::PlausibleManagedObjectGate("installdomain.pregrant", object)) {
                    continue;
                }
                if (!object->IsValidObject() || !object->HasRefField()) {
                    continue;
                }
                object->ForEachRefField(ensureField);
            }
            // Roots after PrepareForwardable (pass2 surface).
            RootVisitor rootEnsure = [this, &ensureObj](ObjectRef& root) {
                zaddress_unsafe observed = root.LoadPlain();
                HeapSlot<> bits(to_zpointer(raw(observed)));
                ensureObj(to_object(bits.GetTargetObject()));
            };
            MutatorManager::Instance().VisitAllMutators(
                [&rootEnsure](Mutator& mutator) { mutator.VisitMutatorRoots(rootEnsure); });
            Heap::GetHeap().VisitStaticRoots(rootEnsure);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rootEnsure);
            collectorResources.GetFinalizerProcessor().VisitRawPointers(rootEnsure);
            Heap::GetHeap().VisitAllExportRoots(rootEnsure);

            size_t grant = g_installDomainGrant.load(std::memory_order_relaxed);
            size_t already = g_installDomainAlready.load(std::memory_order_relaxed);
            size_t tooLate = g_installDomainTooLate.load(std::memory_order_relaxed);
            size_t skip = g_installDomainSkip.load(std::memory_order_relaxed);
            // Always emit (RTLOG_ERROR) so measure captures positive control without VLOG gate.
            LOG(RTLOG_ERROR,
                "[GCV2][installdomain] pregrant grant=%zu already=%zu tooLate=%zu skip=%zu",
                grant, already, tooLate, skip);
        }

        // Reset CAS counters for this fix window (positive-control visibility).
        g_minorRefCasFail.store(0, std::memory_order_relaxed);
        g_minorRefCasOk.store(0, std::memory_order_relaxed);

        if (!useParallel) {
            VLOG(REPORT, "[GCV2][reffix][parallel] fallback=serial pool_unavailable");
            // pass1 roots already done; only heap+remset+pass2 roots remain.
            // Mirror serial fixForwardedReferences but roots again (same as before).
            fixForwardedReferencesSerial();
            VLOG(REPORT,
                 "[GCV2][reffix][parallel] workers_active=1 workers_scheduled=1 objects_taken=[%zu] "
                 "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=0",
                 reachableVec.size(), reachableVec.size(), remsetVec.size(),
                 g_minorRefCasOk.load(std::memory_order_relaxed),
                 g_minorRefCasFail.load(std::memory_order_relaxed));
        } else {
            fixForwardedReferencesParallel(threadPool);
        }

        // fixinput positive control: reject/unrecoverable >0 when tip-in-heap hits Fix.
        {
            size_t rej = g_fixinputReject.load(std::memory_order_relaxed);
            size_t rec = g_fixinputRecover.load(std::memory_order_relaxed);
            size_t unr = g_fixinputUnrecoverable.load(std::memory_order_relaxed);
            if (rej != 0 || rec != 0 || unr != 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][fixinput] reject=%zu recover=%zu unrecoverable=%zu",
                    rej, rec, unr);
            }
        }

        ValidateMinorReferences("before-return", &reachableVec);
        // Mid-evac checkpoint: after slot fix, before region reclaim.
        postEvacPoint("post-fix-pre-forward", true);
    }

    {
        // minortime: ⑥ copy / forward
        MRT_PHASE_TIMER("young.copy");
        ForwardFromSpace();
        postEvacPoint("post-forward-pre-reclaim", true);
        {
            const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
            if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
                ValidateMinorReferences("post-forward-pre-reclaim", &reachableVec);
            }
        }
    }

    {
        // minortime: ⑧ finish inside evacuate (promote residual + remset rebuild + reassemble)
        MRT_PHASE_TIMER("young.evac_finish");
        size_t residualPromoteRecords = 0;
        // Positive-control only (rebuildgate): force one live young region so the
        // rebuild gate must open. Prefer leaving a residual young undemoted; if
        // residualPromote path is empty (product real_load: residual≡0), re-tag
        // the first minor candidate as young after demote. Default off.
        const char* keepOneYoungEnv = std::getenv("MRT_GCV2_REBUILD_KEEP_ONE_YOUNG");
        const bool keepOneYoung =
            keepOneYoungEnv != nullptr && std::strcmp(keepOneYoungEnv, "1") == 0;
        bool keptOneYoung = false;
        for (RegionInfo* region : minorCandidateRegions) {
            if (region->IsYoungRegion()) {
                if (keepOneYoung && !keptOneYoung) {
                    keptOneYoung = true;
                    continue;
                }
                // Residual candidates not forwarded above (e.g. raw-pointer pinned):
                // still demote to old; must replay young→young edges that become old→young.
                region->PreserveRetainedLiveInfo();
                residualPromoteRecords += RegionManager::RecordPromotedCrossGenEdges(region);
                region->SetYoungRegionFlag(0);
                region->SetYoungAge(0);
            }
        }
        if (keepOneYoung && !keptOneYoung) {
            // No residual young remained (common today). Re-tag one candidate so
            // GetYoungRegionCount()>0 and the structural gate opens for the dual-arm
            // positive control. Not a product path.
            for (RegionInfo* region : minorCandidateRegions) {
                if (region == nullptr) {
                    continue;
                }
                region->SetYoungRegionFlag(1);
                keptOneYoung = true;
                VLOG(REPORT,
                     "[GCV2Minor][rebuild-gate] positive-control reyoung region=%p",
                     region);
                break;
            }
        }
        size_t promotedPathRecords = RegionManager::ConsumePromotedCrossGenEdgeCount();

        // R1 structural gate (MINOR_CONCURRENCY_0805 §9.5): after residual demote,
        // live young region count is the product-path authority
        // (RegionInfo::youngRegionCount / GetYoungRegionCount —
        // RegionManager.cpp:185-209; VerifyRegions.cpp:325). When count==0 every
        // holder is non-young and every target is non-young ⇒ rebuild walk is pure
        // cost with zero output. P2 in-place aging reintroduces live young ⇒ gate
        // reopens automatically (structure, not an env switch).
        RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
        size_t rebuiltRecords = 0;
        const size_t liveYoungRegions = RegionInfo::GetYoungRegionCount();
        if (liveYoungRegions == 0) {
            VLOG(REPORT,
                 "[GCV2Minor][rebuild-gate] skip rebuild youngRegionCount=0");
        } else {
            for (BaseObject* object : reachableVec) {
                BaseObject* holder = currentObject(object);
                RegionInfo* holderRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder));
                if (holderRegion->IsYoungRegion() || !holder->HasRefField()) {
                    continue;
                }
                holder->ForEachRefField([this, &rememberedSet, &rebuiltRecords](RefField<>& field) {
                    BaseObject* target = ResolveMinorReference(field);
                    if (!Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion->IsYoungRegion()) {
                        rememberedSet.Record(reinterpret_cast<MAddress>(&field));
                        ++rebuiltRecords;
                    }
                });
            }
            if (rebuiltRecords == 0) {
                VLOG(REPORT,
                     "[GCV2Minor][rebuild-gate] anomaly open-gate-zero-output "
                     "youngRegionCount=%zu",
                     liveYoungRegions);
            }
        }
        VLOG(REPORT,
             "[GCV2Minor] remembered-set rebuilt=%zu promoteReplay=%zu residualPromote=%zu "
             "youngRegionCount=%zu",
             rebuiltRecords, promotedPathRecords, residualPromoteRecords, liveYoungRegions);

        fwdTable.PrepareForwardTable();
        ValidateMinorReferences("after-dispel", nullptr);
        manager.ReassembleFromSpace();
    }
}

void WCollector::ValidateMinorReferences(const char* point, const std::vector<BaseObject*>* reachableVec)
{
    const char* enabled = std::getenv("MRT_GCV2_STALE_REFERENCE_VALIDATOR");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    constexpr size_t categoryCount = 12;
    constexpr size_t sampleCount = 3;
    const std::array<const char*, categoryCount> categoryNames = {
        "stack", "register", "derived", "static", "heap", "weak", "finalizer", "export",
        "concurrency", "external_resurrection", "exception", "raw_object"
    };
    std::array<size_t, categoryCount> counts{};
    std::array<std::array<const void*, sampleCount>, categoryCount> slots{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> holders{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> targets{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> regionTypes{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> objectStates{};
    std::array<std::array<uint16_t, sampleCount>, categoryCount> tags{};
    WorkStack pending = NewWorkStack();
    MinorObjectSet visited;
    bool buildReachableClosure = reachableVec == nullptr;

    auto record = [this, &counts, &slots, &holders, &targets, &regionTypes, &objectStates, &tags](
                      size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (!Heap::IsHeapAddress(target) || !IsGhostFromObject(target) || IsUnmovableFromObject(target)) {
            return false;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        bool regionReturned = region == nullptr || region->IsGarbageRegion() || region->IsFreeRegion();
        ObjectState::ObjectStateCode state = target->GetStateWord().GetStateCode();
        if (!regionReturned && state != ObjectState::FORWARDED) {
            return false;
        }
        size_t sample = counts[category]++;
        if (sample < sampleCount) {
            slots[category][sample] = slot;
            holders[category][sample] = holder;
            targets[category][sample] = target;
            regionTypes[category][sample] =
                region == nullptr ? std::numeric_limits<uint8_t>::max() : static_cast<uint8_t>(region->GetRegionType());
            objectStates[category][sample] = static_cast<uint8_t>(state);
            tags[category][sample] = tag;
        }
        return true;
    };
    auto inspectTarget = [&record, &pending, buildReachableClosure](
                             size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (record(category, slot, holder, target, tag)) {
            return;
        }
        if (!buildReachableClosure || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region != nullptr && !region->IsGarbageRegion() && !region->IsFreeRegion() && target->IsValidObject()) {
            pending.push_back(target);
        }
    };
    auto recordRawRoot = [&inspectTarget](size_t category) {
        return RootVisitor([category, &inspectTarget](ObjectRef& root) {
            HeapSlot<> value(to_zpointer(raw(root.LoadPlain())));
            uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
            inspectTarget(category, &root, nullptr, to_object(value.GetTargetObject()), tag);
        });
    };
    auto recordField = [&inspectTarget](size_t category, BaseObject* holder, RefField<>& field) {
        RefField<> value(field);
        uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
        inspectTarget(category, &field, holder, to_object(value.GetTargetObject()), tag);
    };

    RootVisitor stackVisitor = recordRawRoot(0);
    RootVisitor registerVisitor = recordRawRoot(1);
    DerivedPtrVisitor derivedVisitor = [&inspectTarget](BasePtrType basePtr, DerivedSlot& derivedPtr) {
        inspectTarget(2, &derivedPtr, nullptr, from_native_ref(raw(basePtr)),
                      std::numeric_limits<uint16_t>::max());
    };
    RootVisitor exceptionVisitor = recordRawRoot(10);
    RootVisitor rawObjectVisitor = recordRawRoot(11);
    MutatorManager::Instance().VisitAllMutators(
        [&registerVisitor, &stackVisitor, &derivedVisitor, &exceptionVisitor, &rawObjectVisitor](Mutator& mutator) {
            mutator.VisitHeapReferences(
                registerVisitor, stackVisitor, derivedVisitor, exceptionVisitor, rawObjectVisitor);
        });

    Heap::GetHeap().VisitStaticRoots(recordRawRoot(3));
    collectorResources.GetFinalizerProcessor().VisitRawPointers(recordRawRoot(6));
    Heap::GetHeap().VisitAllExportRoots(recordRawRoot(7));
    RootVisitor concurrencyVisitor = recordRawRoot(8);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&concurrencyVisitor);

    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        for (BaseObject* const& object : resurrectedExportObjectes) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
        for (BaseObject* const& object : resurrectedExportObjectesForwardPhase) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
    }
    {
        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
        for (const auto& entry : cycleRefWorkStack) {
            inspectTarget(9, &entry.first, nullptr, entry.first, std::numeric_limits<uint16_t>::max());
            for (BaseObject* const& object : entry.second) {
                inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
            }
        }
    }

    auto visitObject = [this, &recordField](BaseObject* object) {
        BaseObject* holder = object;
        if (IsGhostFromObject(holder) && !IsUnmovableFromObject(holder) &&
            holder->GetStateWord().GetStateCode() == ObjectState::FORWARDED) {
            holder = FindLatestVersion(holder);
        }
        if (holder == nullptr || IsGhostFromObject(holder) || !holder->IsValidObject() || !holder->HasRefField()) {
            return;
        }
        size_t category = holder->IsWeakRef() ? 5 : 4;
        holder->ForEachRefField(
            [category, holder, &recordField](RefField<>& field) { recordField(category, holder, field); });
    };
    if (reachableVec != nullptr) {
        for (BaseObject* object : *reachableVec) {
            visitObject(object);
        }
    } else {
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (visited.insert(object).second) {
                visitObject(object);
            }
        }
    }

    size_t total = 0;
    for (size_t category = 0; category < categoryCount; ++category) {
        total += counts[category];
        VLOG(REPORT,
             "[GCV2Minor] STALE_SLOT_CATEGORY_%s point=%s count=%zu "
             "samples=[%p/%p/%p/type=%u/state=%u/tag=%u,%p/%p/%p/type=%u/state=%u/tag=%u,"
             "%p/%p/%p/type=%u/state=%u/tag=%u]",
             categoryNames[category], point, counts[category], slots[category][0], holders[category][0],
             targets[category][0], static_cast<unsigned>(regionTypes[category][0]),
             static_cast<unsigned>(objectStates[category][0]), static_cast<unsigned>(tags[category][0]),
             slots[category][1], holders[category][1], targets[category][1],
             static_cast<unsigned>(regionTypes[category][1]), static_cast<unsigned>(objectStates[category][1]),
             static_cast<unsigned>(tags[category][1]), slots[category][2], holders[category][2], targets[category][2],
             static_cast<unsigned>(regionTypes[category][2]), static_cast<unsigned>(objectStates[category][2]),
             static_cast<unsigned>(tags[category][2]));
    }
    VLOG(REPORT, "[GCV2Minor] VALIDATOR_GATED_BY_MRT_GCV2_STALE_REFERENCE_VALIDATOR point=%s total=%zu",
         point, total);
    if (std::strcmp(point, "round2-start") == 0) {
        VLOG(REPORT, "[GCV2Minor] STALE_SLOT_AT_ROUND2_START_%zu", total);
    }
}

void WCollector::VerifyRegionSets(const char* point)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    size_t youngRunIndex = minorTotalRuns + 1;
    if (std::strcmp(point, "after-young-mark") == 0) {
        VerifyRegions::VerifyAfterYoungMark(manager, minorCandidateRegions, youngRunIndex, point);
    } else {
        VerifyRegions::VerifyAfterPrepareYoung(manager, minorCandidateRegions, youngRunIndex, point);
    }
}

void WCollector::ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots)
{
    // Default off. When on: independent full-heap retrace from roots (no remset filter),
    // collect young objs reachable that way, compare to region mark bitmap after young-only mark.
    // For each unmarked-but-full-reachable young object, scan non-young holders for incoming
    // old→young edges and report whether that field is in the minor-acquired remset.
    const char* enabled = std::getenv("MRT_GCMARKGAP_PROBE");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    MinorObjectSet fullReachable;
    MinorObjectSet fullYoung;
    WorkStack pending = NewWorkStack();
    VisitMinorRoots([&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back(object);
        }
    });
    for (BaseObject* object : allocationRoots) {
        pending.push_back(object);
    }
    auto pushField = [this, &pending](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (Heap::IsHeapAddress(target)) {
            pending.push_back(target);
        }
    };
    while (!pending.empty()) {
        BaseObject* object = pending.back();
        pending.pop_back();
        if (!Heap::IsHeapAddress(object) || !fullReachable.insert(object).second) {
            continue;
        }
        if (!object->IsValidObject()) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsYoungRegion()) {
            fullYoung.insert(object);
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            HeapSlot<>& referentField =
                HeapSlotAt<>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = ResolveMinorReference(referentField);
            if (Heap::IsHeapAddress(referent)) {
                referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
            }
            continue;
        }
        object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
    }

    size_t unmarkedLive = 0;
    size_t markedYoung = 0;
    size_t missingEdgeHolders = 0;
    size_t edgeInRemset = 0;
    size_t edgeNotInRemset = 0;
    size_t noIncomingOldFound = 0;
    size_t sampleLimit = 8;
    size_t samples = 0;

    for (BaseObject* object : fullYoung) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsMarkedObject(object)) {
            ++markedYoung;
            continue;
        }
        ++unmarkedLive;

        // Find old→young incoming edges by independent non-young holder walk.
        size_t incomingOld = 0;
        size_t incomingMissing = 0;
        MAddress sampleField = 0;
        BaseObject* sampleHolder = nullptr;
        Heap::GetHeap().ForEachObj(
            [&](BaseObject* holder) {
                if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                    return;
                }
                RegionInfo* hReg = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
                if (hReg == nullptr || hReg->IsYoungRegion() || hReg->IsGarbageRegion() || hReg->IsFreeRegion()) {
                    return;
                }
                holder->ForEachRefField([&](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    if (target != object) {
                        return;
                    }
                    ++incomingOld;
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    bool inRemset = rememberedSlots.count(slot) != 0;
                    if (inRemset) {
                        ++edgeInRemset;
                    } else {
                        ++edgeNotInRemset;
                        ++incomingMissing;
                        if (sampleField == 0) {
                            sampleField = slot;
                            sampleHolder = holder;
                        }
                    }
                });
            },
            false);

        if (incomingMissing > 0) {
            ++missingEdgeHolders;
        }
        if (incomingOld == 0) {
            ++noIncomingOldFound;
        }

        if (samples < sampleLimit) {
            ++samples;
            TypeInfo* ti = object->IsValidObject() ? object->GetTypeInfo() : nullptr;
            TypeInfo* hti = (sampleHolder != nullptr && sampleHolder->IsValidObject()) ? sampleHolder->GetTypeInfo()
                                                                                      : nullptr;
            VLOG(REPORT,
                 "[GCMARKGAP][unmarked-live] run=%zu obj=%p region=%p start=%#zx marked=0 "
                 "fullReachable=1 incomingOld=%zu incomingMissing=%zu sampleField=%p sampleHolder=%p "
                 "objTi=%p holderTi=%p inRemsetSample=%u",
                 minorTotalRuns + 1, object, region, region->GetRegionStart(), incomingOld, incomingMissing,
                 reinterpret_cast<void*>(sampleField), sampleHolder, ti, hti,
                 static_cast<unsigned>(sampleField != 0 && rememberedSlots.count(sampleField) != 0));
        }
    }

    // Also count residual unmarked valid objs on candidates (may be truly dead).
    size_t residualUnmarkedValid = 0;
    size_t residualUnmarkedAndFullReachable = 0;
    size_t neverExaminedCandidates = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        if (region->GetMarkBitmap() == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart()) {
            ++neverExaminedCandidates;
        }
        region->VisitAllObjects([&](BaseObject* object) {
            if (region->IsMarkedObject(object)) {
                return;
            }
            if (!object->IsValidObject()) {
                return;
            }
            ++residualUnmarkedValid;
            if (fullYoung.count(object) != 0) {
                ++residualUnmarkedAndFullReachable;
            }
        });
    }

    VLOG(REPORT,
         "[GCMARKGAP][summary] run=%zu fullYoung=%zu markedYoung=%zu UNMARKED_LIVE=%zu "
         "missingEdgeHolders=%zu edgeInRemset=%zu edgeNotInRemset=%zu noIncomingOld=%zu "
         "residualUnmarkedValid=%zu residualUnmarkedAndFullReachable=%zu neverExaminedCandidates=%zu "
         "remsetSize=%zu env=MRT_GCMARKGAP_PROBE=1",
         minorTotalRuns + 1, fullYoung.size(), markedYoung, unmarkedLive, missingEdgeHolders, edgeInRemset,
         edgeNotInRemset, noIncomingOldFound, residualUnmarkedValid, residualUnmarkedAndFullReachable,
         neverExaminedCandidates, rememberedSlots.size());
}

void WCollector::ValidateYoungMarking(const std::vector<BaseObject*>& reachableVec,
                                      const MinorObjectSet& allocationRoots)
{
    // Gate mirrors ValidateMinorReferences. Default OFF — product path must not abort.
    // Env: MRT_GCV2_VERIFY_YOUNG_MARKING=1 to enable (default unset/other = off).
    // Mark source: MRT_GCV2_VERIFY_MARK_SOURCE (HotSpot VerifyOption isomorphic).
    // Default IndependentVsBitmap — does NOT require MinorClosure membership, so fullYoungScan
    // is not tautological (gcvheap / HotSpot inventory #22).
    const char* enabled = std::getenv("MRT_GCV2_VERIFY_YOUNG_MARKING");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    VerifyMarkSource markSource = ParseVerifyMarkSource();
    const bool useIndependent = markSource == VerifyMarkSource::IndependentVsBitmap ||
                                markSource == VerifyMarkSource::IndependentRetrace ||
                                markSource == VerifyMarkSource::MinorClosure;
    const bool useBitmap = markSource == VerifyMarkSource::IndependentVsBitmap ||
                           markSource == VerifyMarkSource::RegionMarkBitmap ||
                           markSource == VerifyMarkSource::MinorClosure;
    const bool requireMinorClosure = markSource == VerifyMarkSource::MinorClosure;

    MinorObjectSet reachable;
    MinorObjectSet expectedYoung;
    MinorObjectSet reachableSet;
    if (requireMinorClosure) {
        for (BaseObject* object : reachableVec) {
            reachableSet.insert(object);
        }
    }
    if (useIndependent) {
        WorkStack pending = NewWorkStack();
        VisitMinorRoots([&pending](BaseObject* object) {
            if (Heap::IsHeapAddress(object)) {
                pending.push_back(object);
            }
        });
        for (BaseObject* object : allocationRoots) {
            pending.push_back(object);
        }
        auto pushField = [this, &pending](RefField<>& field) {
            BaseObject* target = ResolveMinorReference(field);
            if (Heap::IsHeapAddress(target)) {
                pending.push_back(target);
            }
        };
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (!reachable.insert(object).second) {
                continue;
            }
            CHECK_DETAIL(object->IsValidObject(), "minor marking validator reached invalid object %p", object);
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            if (region->IsYoungRegion()) {
                expectedYoung.insert(object);
            }
            if (!object->HasRefField()) {
                continue;
            }
            if (UNLIKELY(object->IsWeakRef())) {
                HeapSlot<>& referentField = HeapSlotAt<>(
                    reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
                BaseObject* referent = ResolveMinorReference(referentField);
                if (Heap::IsHeapAddress(referent)) {
                    referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
                }
                continue;
            }
            object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
        }
    }

    size_t actualYoung = 0;
    size_t unexpectedYoung = 0;
    if (useBitmap) {
        for (RegionInfo* region : minorCandidateRegions) {
            region->VisitAllObjects([&](BaseObject* object) {
                if (!region->IsMarkedObject(object)) {
                    return;
                }
                ++actualYoung;
                bool bad = false;
                if (useIndependent && expectedYoung.count(object) == 0) {
                    bad = true;
                }
                if (requireMinorClosure && reachableSet.count(object) == 0) {
                    bad = true;
                }
                if (bad) {
                    ++unexpectedYoung;
                }
            });
        }
    }

    size_t missingYoung = 0;
    if (useIndependent) {
        for (BaseObject* object : expectedYoung) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            bool missing = false;
            if (useBitmap && !region->IsMarkedObject(object)) {
                missing = true;
            }
            if (requireMinorClosure && reachableSet.count(object) == 0) {
                missing = true;
            }
            if (missing) {
                ++missingYoung;
            }
        }
    }

    size_t matchCount = (actualYoung >= unexpectedYoung) ? (actualYoung - unexpectedYoung) : 0;
    size_t expectedSize = useIndependent ? expectedYoung.size() : actualYoung;
    VLOG(REPORT,
         "[GCV2][verify][young-marking] run=%zu phase=post-trace env=MRT_GCV2_VERIFY_YOUNG_MARKING=1 "
         "markSource=%s mark-equivalence=%zu/%zu missing=%zu unexpected=%zu "
         "requireMinorClosure=%u",
         minorTotalRuns + 1, VerifyMarkSourceName(markSource), matchCount, expectedSize, missingYoung,
         unexpectedYoung, static_cast<unsigned>(requireMinorClosure));
    if (markSource == VerifyMarkSource::IndependentRetrace || markSource == VerifyMarkSource::RegionMarkBitmap) {
        // Single-source modes only report; cross-check needs two sides.
        return;
    }
    CHECK_DETAIL(missingYoung == 0 && unexpectedYoung == 0 &&
                     (!useIndependent || !useBitmap || actualYoung == expectedYoung.size()),
                 "minor marking differs from full marking: actual=%zu expected=%zu missing=%zu unexpected=%zu "
                 "markSource=%s",
                 actualYoung, expectedYoung.size(), missingYoung, unexpectedYoung,
                 VerifyMarkSourceName(markSource));
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    const bool concurrentStackScan = MutatorManager::ConcurrentStackScanEnabled();
    if (UNLIKELY(!concurrentStackScan && MutatorManager::EpochHandshakeEnabled())) {
        (void)MutatorManager::Instance().RunEpochHandshake("pre-minor");
    }
    std::unique_ptr<ScopedStopTheWorld> stw;
    if (concurrentStackScan) {
        stw = std::make_unique<ScopedStopTheWorld>("young prepare", false);
    } else {
        stw = std::make_unique<ScopedStopTheWorld>("young collection", true, GCPhase::GC_PHASE_ENUM);
    }
    // plaincensus Phase 1a: measure plain HeapSlots before young mark mutates colours.
    RunPlainCensus("pre-minor", false);
    // This STW entry is the young-only mark start; old marking does not participate in a minor.
    flip_young_mark_start();
    // minortime: STW rendezvous cost is already logged by ScopedStopTheWorld dtor
    // ("young collection stw time N us"). Body timers below exclude that wait.
    // Timeline probe (gcdirty): earliest STW point = mutator just handed control.
    // force via POST_EVAC so we do not need global VERIFY_HEAP (avoids pre-evac side effects).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=stw-enter run=%zu priorMinors=%zu",
                 minorTotalRuns + 1, minorTotalRuns);
            VerifyHeapObjects("stw-enter", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=stw-enter run=%zu", minorTotalRuns + 1);
        }
    }
    if (!concurrentStackScan) {
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    }
    {
        // minortime: ① FlushAllocationRegions
        MRT_PHASE_TIMER("young.flush_alloc");
        FlushAllocationRegions();
    }
    if (minorTotalRuns != 0) {
        ValidateMinorReferences("round2-start", nullptr);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        // minortime: ② PrepareYoungGarbageCandidates
        MRT_PHASE_TIMER("young.prepare_candidates");
        stats = manager.PrepareYoungGarbageCandidates(
            [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    }
    // HotSpot g1HeapVerifier.cpp:424 verify_region_sets placement: after region accounting is stable.
    VerifyRegionSets("after-prepare-young");
    // Region-set verify after candidate construction (HotSpot verify_region_sets placement intent).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-prepare-young run=%zu",
                 minorTotalRuns + 1);
            VerifyHeapObjects("post-prepare-young", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=post-prepare-young run=%zu", minorTotalRuns + 1);
        }
    }
    if (stats.candidateRegions == 0) {
        manager.ReassembleFromSpace();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        ++minorTotalRuns;
        VLOG(REPORT, "[GCV2Minor] run=%zu candidates=0 candidateBytes=0 live=0 reclaimedBytes=0",
             minorTotalRuns);
        return;
    }

    // Pinned holders (Future/Mutex/Monitor): AllocPinned never sets young; IDLE write
    // fast-path (phase < ENUM) is a bare store — old→young edges never hit remset.
    // Stamp them before Acquire so pre-evacuate verify and young mark both see them.
    // idleedge: census remset-miss old→young BEFORE pinned stamp fills those gaps.
    IdleEdgeDiag::CensusPrePinnedStamp(minorTotalRuns + 1);
    MinorSlotSet rememberedSlots;
    {
        // minortime: ④ remset / cross-gen edge consume (drain + pinned stamp; rescan below)
        MRT_PHASE_TIMER("young.remset_drain");
        size_t pinnedRemsetRecords = manager.RecordPinnedCrossGenEdges();
        if (pinnedRemsetRecords != 0) {
            VLOG(REPORT, "[GCV2Minor] pinnedCrossGenEdges=%zu", pinnedRemsetRecords);
        }
        Heap::GetHeap().GetRememberedSet().DrainForMinor(rememberedSlots);
    }

    uint64_t stackScanEpoch = 0;
    if (concurrentStackScan) {
        // Publish S1/S3/S5 while every mutator is stopped. SetGCPhase is the
        // release publication point; AcknowledgeEpochHandshake asserts ENUM
        // before it is allowed to snapshot a single frame.
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_ENUM);
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_ENUM);
        stw.reset();

        EpochHandshakeStats handshake = MutatorManager::Instance().RunEpochHandshake("pre-minor-stack");
        stackScanEpoch = handshake.epoch;
        CHECK_DETAIL(stackScanEpoch != 0 && handshake.stackScanned + handshake.stackFallback == handshake.requested,
                     "minor concurrent stack scan accounting failed: epoch=%llu requested=%zu scanned=%zu "
                     "fallback=%zu",
                     static_cast<unsigned long long>(stackScanEpoch), handshake.requested, handshake.stackScanned,
                     handshake.stackFallback);

        // CLEAR is the closing edge for ENUM writes: it flushes every mutator's
        // SATB node before the root pass consumes retired objects below.
        stw = std::make_unique<ScopedStopTheWorld>("young collection", false);
        TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);

        // An incomplete concurrent cursor (RootMap miss or missing managed
        // bounds) cannot replace the legacy phase-enum path: GcPhaseEnum also
        // traverses stack-allocated objects through CheckAndPush, while the raw
        // root pass below only sees direct heap roots. Re-run that exact path
        // under the closing STW before alloc-buffer roots are merged.
        MutatorManager::Instance().VisitAllMutators([stackScanEpoch](Mutator& mutator) {
            if (!mutator.GetStackWatermark().IsDone(stackScanEpoch)) {
                (void)mutator.GcPhaseEnum(GCPhase::GC_PHASE_ENUM);
            }
        });
    }

    const char* fallback = std::getenv("MRT_GCV2_FULL_YOUNG_SCAN");
    bool fullYoungScan = fallback == nullptr || std::strcmp(fallback, "0") != 0;
    // setbitmap O1③: default ON (bitmap claim + vector). MRT_GCV2_SETBITMAP=0 → legacy set path.
    static const bool useBitmapLedger = []() {
        const char* v = std::getenv("MRT_GCV2_SETBITMAP");
        if (v != nullptr && std::strcmp(v, "0") == 0) {
            return false;
        }
        return true;
    }();
    WorkStack workStack = NewWorkStack();
    MinorObjectSet reachableObjects; // legacy set path + FYS non-young holders under bitmap
    std::vector<BaseObject*> reachableVec;
    reachableVec.reserve(1 << 17); // ~128k; real_load ~155k reachable
    MinorObjectSet allocationRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    {
        // minortime: ③ root enum (alloc buffers + VisitMinorRoots)
        MRT_PHASE_TIMER("young.root_enum");
        WorkStack enumRoots = NewWorkStack();
        theAllocator.VisitAllocBuffers([&enumRoots](AllocBuffer& buffer) { buffer.MergeRoots(enumRoots); });
        if (stackScanEpoch != 0) {
            SatbBuffer::Instance().GetRetiredObjects(enumRoots);
        }
        while (!enumRoots.empty()) {
            BaseObject* object = enumRoots.back();
            enumRoots.pop_back();
            if (Heap::IsHeapAddress(object)) {
                allocationRoots.insert(object);
            }
            if (fullYoungScan) {
                if (Heap::IsHeapAddress(object)) {
                    workStack.push_back(object);
                }
            } else {
                PushYoungObject(object, workStack, "alloc_buffer");
            }
        }
        VisitMinorRoots([this, fullYoungScan, &workStack](BaseObject* object) {
            if (fullYoungScan) {
                if (Heap::IsHeapAddress(object)) {
                    workStack.push_back(object);
                }
            } else {
                // origin comes from gMinorRootOrigin set inside VisitMinorRootSlots/ValueRoots
                PushYoungObject(object, workStack, "minor_root");
            }
        }, stackScanEpoch);
    }
    // youngconc: concurrent young mark (mutator-concurrent, not only STW-parallel).
    // Default OFF until STW2 remset/root fixpoint is checksum-clean (see REPORT-youngconc).
    // MRT_GCV2_YOUNG_CONC_MARK=1 enables; reuses major TRACE barrier + SATB (no second family).
    // STW1 = prepare + remset drain + root enum; STW2 = concurrent remset drain + re-enum + evacuate.
    static const bool youngConcMark = []() {
        const char* v = std::getenv("MRT_GCV2_YOUNG_CONC_MARK");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    if (youngConcMark && stw != nullptr) {
        // Publish TRACE while mutators are still stopped so resume sees TraceBarrier/SATB.
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        stw.reset();
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark start (mutators running)");
    }
    g_minorLedgerCost.Reset();
    {
        // minortime: ⑤ mark closure pass-1 (from roots)
        MRT_PHASE_TIMER("young.mark_closure");
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger);
    }
    MinorSlotSet liveRememberedSlots;
    for (MAddress slot : rememberedSlots) {
        if (LedgerCount(weakSlots, slot, g_minorLedgerCost.weakLookN, g_minorLedgerCost.weakLookNs) == 0 &&
            (!fullYoungScan ||
             LedgerCount(reachableSlots, slot, g_minorLedgerCost.slotLookN, g_minorLedgerCost.slotLookNs) != 0)) {
            liveRememberedSlots.insert(slot);
        }
    }
    // Remset consume-vs-recorded (G1SummarizeRSetStats analog) + optional dual-closure
    // diff-path explainer. Both gated default-off; see DiffPathExplainer.h.
    DiffPathRemsetStats remsetStats;
    remsetStats.recorded = rememberedSlots.size();
    remsetStats.live = liveRememberedSlots.size();
    MinorSlotSet consumedSlots;
    {
        // minortime: ④ remset rescan + ⑤ mark closure pass-2 (from remset edges)
        MRT_PHASE_TIMER("young.remset_rescan");
        RescanRememberedSet(workStack, rememberedSlots, reachableSlots, weakSlots, fullYoungScan, &consumedSlots,
                            &remsetStats);
    }
    {
        MRT_PHASE_TIMER("young.mark_from_remset");
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                          useBitmapLedger);
    }
    if (youngConcMark) {
        // SATB termination while concurrent (major MarkSatbBuffer shape). Ends in CLEAR_SATB.
        CHECK_DETAIL(MarkYoungSatbBuffer(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                         weakSlots, useBitmapLedger),
                     "young concurrent mark SATB not cleared");
        // STW2: freeze world for post-mark verify + evacuate (still STW today).
        stw = std::make_unique<ScopedStopTheWorld>("young post-mark", false);
        // Concurrent window recorded new old→young edges on the active remset face.
        // Drain them under STW and fold into the same mark/remset ledger used by evacuate.
        {
            MRT_PHASE_TIMER("young.remset_drain_conc");
            MinorSlotSet concurrentRemset;
            size_t nConc = Heap::GetHeap().GetRememberedSet().DrainForMinor(concurrentRemset);
            if (nConc != 0) {
                VLOG(REPORT, "[GCV2][youngconc] concurrent remset drained=%zu", nConc);
                rememberedSlots.insert(concurrentRemset.begin(), concurrentRemset.end());
                remsetStats.recorded = rememberedSlots.size();
                RescanRememberedSet(workStack, concurrentRemset, reachableSlots, weakSlots, fullYoungScan,
                                    &consumedSlots, &remsetStats);
                TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                  useBitmapLedger);
            }
        }
        // Re-enum roots under STW2: stacks/statics may have gained young refs while concurrent.
        {
            MRT_PHASE_TIMER("young.root_enum_final");
            WorkStack finalRoots = NewWorkStack();
            theAllocator.VisitAllocBuffers([&finalRoots](AllocBuffer& buffer) { buffer.MergeRoots(finalRoots); });
            SatbBuffer::Instance().GetRetiredObjects(finalRoots);
            while (!finalRoots.empty()) {
                BaseObject* object = finalRoots.back();
                finalRoots.pop_back();
                if (Heap::IsHeapAddress(object)) {
                    allocationRoots.insert(object);
                }
                if (fullYoungScan) {
                    if (Heap::IsHeapAddress(object)) {
                        workStack.push_back(object);
                    }
                } else {
                    PushYoungObject(object, workStack, "alloc_buffer_final");
                }
            }
            VisitMinorRoots([this, fullYoungScan, &workStack](BaseObject* object) {
                if (fullYoungScan) {
                    if (Heap::IsHeapAddress(object)) {
                        workStack.push_back(object);
                    }
                } else {
                    PushYoungObject(object, workStack, "minor_root_final");
                }
            });
            if (!workStack.empty()) {
                TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                  useBitmapLedger);
            }
        }
        // Final SATB drain under STW so no mutator write is in flight past this point.
        {
            WorkStack finalSatb = NewWorkStack();
            SatbBuffer::Instance().GetRetiredObjects(finalSatb);
            while (!finalSatb.empty()) {
                BaseObject* obj = finalSatb.back();
                finalSatb.pop_back();
                if (!Heap::IsHeapAddress(obj)) {
                    continue;
                }
                if (fullYoungScan) {
                    workStack.push_back(obj);
                } else {
                    PushYoungObject(obj, workStack, "young_satb_final");
                }
            }
            if (!workStack.empty()) {
                TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableVec, reachableSlots, weakSlots,
                                  useBitmapLedger);
            }
        }
        // Rebuild liveRememberedSlots after concurrent remset merge (evac uses this set).
        liveRememberedSlots.clear();
        for (MAddress slot : rememberedSlots) {
            if (LedgerCount(weakSlots, slot, g_minorLedgerCost.weakLookN, g_minorLedgerCost.weakLookNs) == 0 &&
                (!fullYoungScan ||
                 LedgerCount(reachableSlots, slot, g_minorLedgerCost.slotLookN, g_minorLedgerCost.slotLookNs) != 0)) {
                liveRememberedSlots.insert(slot);
            }
        }
        remsetStats.live = liveRememberedSlots.size();
        VLOG(REPORT, "[GCV2][youngconc] concurrent young mark done; STW2 post-mark+evac reachable=%zu",
             reachableVec.size());
    }
    g_minorLedgerCost.Report();
    VLOG(REPORT, "[GCV2][setbitmap] use=%d reachable_n=%zu set_n=%zu fullYoung=%d youngConc=%d",
         static_cast<int>(useBitmapLedger), reachableVec.size(), reachableObjects.size(),
         static_cast<int>(fullYoungScan), static_cast<int>(youngConcMark));
    // blackmark: fixpoint edges from already-reachable holders to unmarked young objects.
    // PrepareYoung ClearLiveInfo drops any pre-mark allocation-black bits; objects allocated
    // into candidate regions (or still living there) that are only reached from live holders
    // after the root/remset wave can still be live0Surv=0 at GetRoute. Walk reachableVec
    // fields once more and re-enter TraceYoungClosure for newly claimed young targets.
    // Default OFF (same switch as alloc paint). Incomplete: ALOT still 10/10 route miss.
    {
        static const bool blackmarkFixOn = []() {
            const char* v = std::getenv("MRT_GCV2_ALLOC_BLACK");
            return v != nullptr && v[0] == '1' && v[1] == '\0';
        }();
        if (blackmarkFixOn) {
            WorkStack blackmarkExtra = NewWorkStack();
            const size_t nHolders = reachableVec.size();
            for (size_t i = 0; i < nHolders; ++i) {
                BaseObject* object = reachableVec[i];
                if (object == nullptr || !Heap::IsHeapAddress(object)) {
                    continue;
                }
                if (!Collector::PlausibleManagedObjectGate("blackmark.fixpoint.holder", object)) {
                    continue;
                }
                if (!object->HasRefField()) {
                    continue;
                }
                object->ForEachRefField([this, &blackmarkExtra](RefField<>& field) {
                    BaseObject* target = ResolveMinorReference(field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    if (!Collector::PlausibleManagedObjectGate("blackmark.fixpoint.target", target)) {
                        BaseObject* host = Collector::TryRecoverInteriorBase(target);
                        if (host != nullptr && host != target) {
                            target = host;
                        } else {
                            return;
                        }
                    }
                    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (region == nullptr || !region->IsYoungRegion()) {
                        return;
                    }
                    if (region->IsMarkedObject(target)) {
                        return;
                    }
                    // Candidate / from / recent-full young: any young that can enter GetRoute.
                    blackmarkExtra.push_back(target);
                });
            }
            if (!blackmarkExtra.empty()) {
                size_t before = reachableVec.size();
                TraceYoungClosure(blackmarkExtra, fullYoungScan, reachableObjects, reachableVec, reachableSlots,
                                  weakSlots, useBitmapLedger);
                VLOG(REPORT, "[GCV2][blackmark] fixpoint_extra_roots=%zu reachable_before=%zu after=%zu",
                     blackmarkExtra.size(), before, reachableVec.size());
            }
        }
    }
    // setbitmap2: optional closure equality probe (default off).
    // mode=1: dump product ptr-set hash; mode=2: in-process dual legacy set walk on same roots.
    ClosureHashProbe::ReportDump(minorTotalRuns + 1, reachableVec, useBitmapLedger, fullYoungScan);
    if (ClosureHashProbe::Dual()) {
        // Independent set claim on the *same* STW root/remset snapshot.
        // FYS=1: reuse TraceYoungClosure + RescanRememberedSet (set claim) — mark bits don't
        // gate set insert. FYS=0: PushYoungObject skips already-marked young, so use a
        // mark-agnostic set walker (same roots + remset targets).
        std::vector<BaseObject*> dualVec;
        dualVec.reserve(reachableVec.size());
        if (fullYoungScan) {
            WorkStack dualStack = NewWorkStack();
            for (BaseObject* object : allocationRoots) {
                if (Heap::IsHeapAddress(object)) {
                    dualStack.push_back(object);
                }
            }
            VisitMinorRoots([&dualStack](BaseObject* object) {
                if (Heap::IsHeapAddress(object)) {
                    dualStack.push_back(object);
                }
            });
            MinorObjectSet dualObjects;
            MinorSlotSet dualSlots;
            MinorSlotSet dualWeaks;
            TraceYoungClosure(dualStack, fullYoungScan, dualObjects, dualVec, dualSlots, dualWeaks,
                              /*useBitmapLedger=*/false);
            RescanRememberedSet(dualStack, rememberedSlots, dualSlots, dualWeaks, fullYoungScan, nullptr, nullptr);
            TraceYoungClosure(dualStack, fullYoungScan, dualObjects, dualVec, dualSlots, dualWeaks,
                              /*useBitmapLedger=*/false);
        } else {
            auto dualPush = [](BaseObject* object, WorkStack& stack) {
                if (!Heap::IsHeapAddress(object)) {
                    return;
                }
                RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
                if (region->IsYoungRegion()) {
                    stack.push_back(object);
                }
            };
            WorkStack dualStack = NewWorkStack();
            for (BaseObject* object : allocationRoots) {
                dualPush(object, dualStack);
            }
            VisitMinorRoots([&dualPush, &dualStack](BaseObject* object) { dualPush(object, dualStack); });
            for (MAddress slot : rememberedSlots) {
                HeapSlot<>& field = HeapSlotAt<>(slot);
                dualPush(ResolveMinorReference(field), dualStack);
            }
            MinorObjectSet dualSeen;
            while (!dualStack.empty()) {
                BaseObject* object = dualStack.back();
                dualStack.pop_back();
                if (!Heap::IsHeapAddress(object)) {
                    continue;
                }
                if (!dualSeen.insert(object).second) {
                    continue;
                }
                RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
                if (!region->IsYoungRegion()) {
                    continue;
                }
                dualVec.push_back(object);
                if (!object->HasRefField()) {
                    continue;
                }
                if (UNLIKELY(object->IsWeakRef())) {
                    HeapSlot<>& referentField = HeapSlotAt<>(
                        reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
                    BaseObject* referent = ResolveMinorReference(referentField);
                    if (Heap::IsHeapAddress(referent)) {
                        referent->ForEachRefField([this, &dualPush, &dualStack](RefField<>& field) {
                            dualPush(ResolveMinorReference(field), dualStack);
                        });
                    }
                    continue;
                }
                object->ForEachRefField([this, &dualPush, &dualStack](RefField<>& field) {
                    dualPush(ResolveMinorReference(field), dualStack);
                });
            }
        }
        ClosureHashProbe::ReportEqual(minorTotalRuns + 1, reachableVec, dualVec, useBitmapLedger, fullYoungScan);
    }
    static const bool verifyRemsetEnabled = []() {
        const char* value = std::getenv("MRT_GCV2_VERIFY_REMSET");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    // Independent remset completeness check (invariant R). Gated by MRT_GCV2_VERIFY_REMSET.
    // Uses the minor-acquired slot set: live remset is empty after AcquireRecordsForMinor.
    {
        size_t runIndex = minorTotalRuns + 1;
        auto visitRoots = [this, &allocationRoots](const std::function<void(BaseObject*)>& visitor) {
            for (BaseObject* object : allocationRoots) {
                visitor(object);
            }
            VisitMinorRoots(visitor);
        };
        auto resolveField = [this](RefField<>& field) -> BaseObject* { return ResolveMinorReference(field); };
        if (verifyRemsetEnabled) {
            std::unordered_set<BaseObject*> rootReachableForRemsetVerify;
            RunDiffPathExplainer(runIndex, visitRoots, resolveField, rememberedSlots, consumedSlots,
                                 &minorCandidateRegions, remsetStats, &rootReachableForRemsetVerify);
            VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, false, &rootReachableForRemsetVerify);
        } else {
            RunDiffPathExplainer(runIndex, visitRoots, resolveField, rememberedSlots, consumedSlots,
                                 &minorCandidateRegions, remsetStats, nullptr);
            VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, false, nullptr);
        }
    }
    // Full-heap object invariant H (HotSpot G1HeapVerifier::verify inventory #10).
    // Independent ForEachObj walk; gated by MRT_GCV2_VERIFY_HEAP (default off).
    // Timeline (gcdirty): also force as post-mark under POST_EVAC so first-dirty bracketing
    // does not require global VERIFY_HEAP.
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-mark run=%zu", minorTotalRuns + 1);
            VerifyHeapObjects("post-mark", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=post-mark run=%zu", minorTotalRuns + 1);
        } else {
            VerifyHeapObjects("pre-evacuate");
        }
    }

    size_t liveBytes = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        liveBytes += region->GetLiveByteCount();
    }
    if (fullYoungScan) {
        // Run structural verify before mark-equivalence CHECK (may abort).
        VerifyRegionSets("after-young-mark");
        ValidateYoungMarking(reachableVec, allocationRoots);
    }
    // Always-available (gated) probe: full-heap independent reachability vs young-only bitmap.
    // Runs with FULL_YOUNG_SCAN=0 so B2 path is exercised. Default off.
    ProbeUnmarkedLive(allocationRoots, rememberedSlots);

    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER("young.pre_evac_clear");
        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
        WeakRefBuffer::Instance().ClearWeakRefBuffer();
        SatbBuffer::Instance().ClearBuffer();
    }

    size_t allocatedBefore = space.AllocatedBytes();
    // ⑥⑦⑧ inside EvacuateYoungRegions: young.ref_fix / young.copy / young.evac_finish
    EvacuateYoungRegions(reachableVec, liveRememberedSlots);
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats().collectedBytes = stats.reclaimedBytes;

    // Post-evacuate invariant P (HotSpot VerifyAfterGC analog for young): after
    // fix+forward+remset rebuild inside EvacuateYoungRegions, every live ref must
    // still be a legal object (VerifyHeap H) and remset must cover old→young (R).
    // Gate default off: MRT_GCV2_VERIFY_POST_EVAC=1. force=true so this does not
    // require MRT_GCV2_VERIFY_HEAP/REMSET (avoids pre-evacuate side effects).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VerifyHeapObjects("post-evacuate", true);
            std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
            VerifyRememberedSetInvariant("post-evacuate", remsetSnap, true);
            ValidateMinorReferences("post-evacuate", nullptr);
            VLOG(REPORT,
                 "[GCV2][verify][post-evac] point=post-evacuate run=%zu "
                 "env=MRT_GCV2_VERIFY_POST_EVAC=1 remsetSnap=%zu",
                 minorTotalRuns + 1, remsetSnap.size());
        }
    }

    {
        // minortime: ⑧ post-evac finish
        MRT_PHASE_TIMER("young.post_evac_finish");
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        MergeResurrectExportObjects();
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveBytes, liveRememberedSlots.size(), stats.reclaimedBytes, pauseUs);
    // csetalloc: surface cumulative "would allocate into CSet" count (always-on counter,
    // zero-cost when no hits; LOG only if non-zero so default noise stays quiet).
    {
        size_t into = RegionSpace::AllocIntoCSetCount();
        size_t retired = RegionSpace::AllocIntoCSetRetiredCount();
        if (into != 0 || retired != 0) {
            VLOG(REPORT, "[GCV2][csetalloc] cumulative intoCSet=%zu retired=%zu (post-minor run=%zu)",
                 into, retired, minorTotalRuns);
        }
    }
    // STEER4: DumpScrubCostAndReset is a no-op unless MRT_GCV2_SCRUB_COST=1.
    RegionManager::DumpScrubCostAndReset("post-minor");
    IdleEdgeDiag::DumpProcessTotals("post-minor");
}

void WCollector::DoGarbageCollection()
{
    if (gcReason == GC_REASON_YOUNG) {
        DoYoungGarbageCollection();
        Collector::ReportMarkGoodHeapGateCounts();
        return;
    }
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();

    // Publish a clean full-GC buffer before mutators return to IDLE. The phase
    // transition is the grace period for writers that had already loaded the old
    // buffer index; clear that captured buffer only after the transition completes.
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    uint8_t fullRemsetBuffer = remset.BeginFullClear();
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    size_t droppedRemsetRecords = remset.FinishFullClear(fullRemsetBuffer);
    if (droppedRemsetRecords != 0) {
        VLOG(REPORT, "[GCV2][remset] cleared after full GC dropped=%zu", droppedRemsetRecords);
    }
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);
    // FlipTagID just turned this cycle's current-tags into IsOldPointer. F3 pre-flip only
    // saw the previous tag generation. This pass must NOT filter IsSurvivedObject:
    // after Forward, live holders are in to-space without mark bits at the new addr.
    //
    // This walk exists because a reference could not say for itself that its colour was stale, so
    // someone had to strip the old tag off every one of them before the tag was reused. Once the
    // read barrier heals a stale colour on the way past (FixOldTaggedRefField), the walk has
    // nothing left to do -- but that claim needs measuring before the walk goes away for good, so
    // it is a switch rather than a deletion. Nobody has measured what this pass costs.
    static const bool skipPostflipWalk = []() {
        const char* v = std::getenv("MRT_GCV2_SKIP_POSTFLIP_WALK");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    if (!skipPostflipWalk) {
        InvalidateOldTaggedRefs(false);
    }

    CollectSmallSpace();
    // retmid: do NOT StampCensusBoundaries / PromoteAllRegions here.
    // Ablation D (both major STWs disabled) restores mid_alloc 5/5; any of
    // Flush/Stamp/Promote in these STWs reintroduces 0/5 or residual 甲 under
    // FYS=0 SKIP_PINNED=1 512MB. Retained-liveness still applies on residual and
    // in-place promote paths that already Preserve + RecordPromotedCrossGenEdges.
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
    Collector::ReportMarkGoodHeapGateCounts();
}

void WCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) { return !IsMarkedObject(obj); };
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    // markfloor: stack/reg roots may hold RawArray+8 interiors (tip=length). Do not
    // GetSize/CopyObject them; leave the slot unchanged (caller keeps obj).
    if (!Collector::PlausibleManagedObjectGate("WCollector::ForwardObject", obj)) {
        return obj;
    }
    BaseObject* to = TryForwardObject(obj);
    return (to != nullptr) ? to : obj;
}

BaseObject* WCollector::TryForwardObject(BaseObject* obj)
{
    if (!Collector::PlausibleManagedObjectGate("WCollector::TryForwardObject", obj)) {
        return nullptr;
    }
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return nullptr;
    }

    if (fwdTable.RouteRegion(region)) {
        // secondclass ①: GetRoute is geometric plan; wait FORWARDED or read-lock
        // before consuming to-slot (else null-tip → HasRefField SEGV si_addr=0x8).
        for (;;) {
            if (region->TryLockReadFromRegion()) {
                BaseObject* toVersion = ForwardObjectImpl(obj, region);
                region->UnlockReadFromRegion();
                return toVersion;
            }
            if (obj->IsForwarded()) {
                return FindToVersion(obj);
            }
            sched_yield();
        }
    } else if (region->IsCompacted()) {
        // Compact copies under region write-lock before COMPACTED is published.
        return FindToVersion(obj);
    }
    return nullptr;
}

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>(%zu)", obj, toObj, toObj->GetTypeInfo(), toObj->GetSize());
            return toObj;
        }

        // 2. object is being forwarded, spin until it is forwarded (or gets its own forwarded address)
        if (oldWord.IsLockedWord()) {
            sched_yield();
            continue;
        }

        // 3. hope we can forward this object
        if (obj->TryLockObject(oldWord)) {
            return ForwardObjectExclusive(obj);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    if (!Collector::PlausibleManagedObjectGate("WCollector::ForwardObjectExclusive", obj)) {
        // Caller locked for a real object; unlock without claiming FORWARDED.
        obj->UnlockObject(ObjectState::NORMAL);
        return nullptr;
    }
    size_t size = RegionSpace::GetAllocSize(*obj);
    BaseObject* toObj = fwdTable.RouteObject(obj);
    CHECK_DETAIL(toObj != nullptr, "invalid object route");
    DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
    CopyObject(*obj, *toObj, size);
    toObj->SetStateCode(ObjectState::NORMAL);
    std::atomic_thread_fence(std::memory_order_release);
    obj->UnlockObject(ObjectState::FORWARDED);
    return toObj;
}

void WCollector::CollectSmallSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    {
        MRT_PHASE_TIMER("CollectFromSpaceGarbage");
        stats.collectedBytes += stats.smallGarbageSize;
        space.CollectFromSpaceGarbage();
    }

    size_t candidateBytes = stats.fromSpaceSize + stats.pinnedSpaceSize + stats.largeSpaceSize;
    stats.garbageRatio = (candidateBytes > 0) ? static_cast<float>(stats.collectedBytes) / candidateBytes : 0;

    stats.liveBytesAfterGC = space.AllocatedBytes();

    VLOG(REPORT,
         "collect %zu B: old small %zu - %zu B, old pinned %zu - %zu B, old large %zu - %zu B. garbage ratio %.2f%%",
         stats.collectedBytes, stats.fromSpaceSize, stats.smallGarbageSize, stats.pinnedSpaceSize,
         stats.pinnedGarbageSize, stats.largeSpaceSize, stats.largeGarbageSize,
         stats.garbageRatio * 100); // The base of the percentage is 100

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    collectorResources.GetFinalizerProcessor().NotifyToReclaimGarbage();
}

bool WCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
