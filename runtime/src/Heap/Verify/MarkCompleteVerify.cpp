// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MarkCompleteVerify.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/InteriorEdgeClass.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace MarkCompleteVerify {
namespace {

constexpr size_t kSampleLimit = 32;
constexpr size_t kTraceWatchSlots = 256;
constexpr size_t kTraceWatchMask = kTraceWatchSlots - 1;

struct TraceWatchSlot {
    std::atomic<uintptr_t> addr{ 0 };
    std::atomic<uint64_t> traced{ 0 };
    std::atomic<uint64_t> moved{ 0 };
};

TraceWatchSlot g_traceWatch[kTraceWatchSlots];
std::atomic<uint64_t> g_traceWatched{ 0 };
std::atomic<uint64_t> g_traceCollisions{ 0 };

size_t TraceWatchSlotOf(uintptr_t addr)
{
    return static_cast<size_t>((addr >> 4) & kTraceWatchMask);
}

struct Stats {
    // Walk coverage -- the positive controls.  A defect count of zero next to a
    // zero here means the walk did not run, not that the heap is clean.
    size_t objectsScanned = 0;
    size_t liveHolders = 0;
    size_t edgesSeen = 0;

    // Accepted, by reason (not defects).
    size_t targetMarked = 0;
    size_t targetYoung = 0;
    size_t targetAllocGap = 0;
    size_t targetInteriorBaseMarked = 0;
    size_t targetNonHeap = 0;
    size_t targetForwarded = 0;

    // Defects: a live holder naming an object this cycle's mark did not mark.
    size_t deadTarget = 0;
    // Of those, the ones whose region `!is_marked` would actually free.  This is
    // the subset that turns into a use-after-free the moment the knife lands.
    size_t deadTargetKnownEmpty = 0;
    // The rollback question for 8eeefdff: would the now-live CSet-empty predicate
    // actually free the page this live holder points into?
    size_t deadTargetWouldFree = 0;
    size_t deadTargetWouldKeep = 0;
    size_t deadTargetNotConsidered = 0;
    // Region shape of the dead target, for triage.
    size_t deadInFrom = 0;
    size_t deadInGarbage = 0;
    size_t deadInFree = 0;
    size_t deadInLarge = 0;
    size_t deadInOther = 0;
    size_t deadNoRegion = 0;
    size_t deadInterior = 0;
    // Nested intedge census (MRT_GCV2_MARKCOMPLETE_INTEDGE). Sum of the four
    // equals deadInterior when the nested gate is on; stays 0 when it is off
    // so a parser that subtracts them from deadInterior cannot invent a drop
    // on the deadFrom arm. Survnode reads deadFrom; these columns must not
    // move it.
    size_t deadIntSlotNotRef = 0;
    size_t deadIntRecoverFail = 0;
    size_t deadIntBaseUnmarked = 0;
    size_t deadIntValueCorrupt = 0;

    std::unordered_map<RegionInfo*, std::vector<std::pair<uintptr_t, size_t>>> streamIndex;
    std::unordered_map<RegionInfo*, unsigned> streamTruncated;

    // Root arm (ZVerify::roots_strong, zVerify.cpp:496-499).
    size_t rootsSeen = 0;
    size_t rootDead = 0;

    // Walk-coverage census.  RegionInfo::VisitAllObjects (RegionManager.cpp:648-663)
    // *breaks* at the first object whose header fails the gate -- "remaining stream
    // is unwalkable" -- so the heap walk this verifier rides on can stop short
    // inside a region.  Without these two numbers a deadTarget=0 could just mean
    // the walk never reached the interesting bytes.
    size_t regionsWalked = 0;
    size_t regionsTruncated = 0;
    size_t bytesUnwalked = 0;

    uint64_t costNs = 0;
    std::array<void*, kSampleLimit> holderSamples{};
    std::array<void*, kSampleLimit> targetSamples{};
    size_t sampleCount = 0;
};

// DoResurrection runs inside DoTracing (TracingCollector.cpp:805), so by the time
// this walk happens a finalizable object can be live without carrying a mark bit.
// TracingCollector::IsSurvivedObject (TracingCollector.h:318-324) is the predicate
// that admits both; using the bare mark bit here would report every reference into
// a resurrected object as a completeness defect.
bool IsLiveOld(const BaseObject* obj)
{
    return RegionSpace::IsMarkedObject<Generation::Old>(obj) || RegionSpace::IsResurrectedObject(obj);
}

const char* RegionKindName(const RegionInfo* region)
{
    if (region == nullptr) {
        return "null";
    }
    if (region->IsFreeRegion()) {
        return "free";
    }
    if (region->IsGarbageRegion()) {
        return "garbage";
    }
    if (region->IsYoungRegion()) {
        return "young";
    }
    if (region->IsFromRegion()) {
        return "from";
    }
    if (region->IsLoneFromRegion()) {
        return "lone-from";
    }
    if (region->IsUnmovableFromRegion()) {
        return "unmovable-from";
    }
    if (region->IsLargeRegion()) {
        return "large";
    }
    return "old-or-other";
}

// Replicates the CSet-empty free predicate (RegionManager.cpp:1479-1517) for one
// region. The address join against sampled [WHODEAD][cset-empty] lines could only
// ever say "no evidence"; those lines are emitted for n<=8 or powers of two, so a
// miss proves nothing. This asks the predicate directly, per dead edge, which is
// what decides whether a page a live holder points into would actually be freed.
// Returns 1 if the predicate would free it, 0 if not, 2 if the guard conditions
// mean the predicate never even considers this region.
unsigned WouldCsetPredicateFree(RegionInfo* region)
{
    if (region == nullptr) {
        return 2;
    }
    // Outer guard (RegionManager.cpp:1479-1482): only from-regions with no live
    // bytes, no raw pointers, no alloc gap and not young reach the predicate.
    if (!region->IsFromRegion() || region->GetLiveByteCount() != 0 || region->GetRawPointerObjectCount() != 0 ||
        region->HasMarkStartAllocGap() || region->IsYoungRegion()) {
        return 2;
    }
    const bool ke = region->IsKnownEmpty(region->GetMarkView<Generation::Old>());
    size_t residual = 0;
    size_t residualFwd = 0;
    size_t marked = 0;
    const uintptr_t start = region->GetRegionStart();
    const uintptr_t alloc = region->GetRegionAllocPtr();
    if (alloc > start && !region->IsLargeRegion()) {
        MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
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
            if (region->IsMarkedObject(view, o)) {
                ++marked;
            }
            pos += sz;
        }
    }
    const bool deadFromCopy = residual == residualFwd;
    const bool unmarkedResidual = residual != 0 && marked == 0;
    return (ke || deadFromCopy || unmarkedResidual) ? 1u : 0u;
}

void ReportDeadEdge(Stats& stats, size_t maxSamples, const char* point, BaseObject* holder, RefField<>& field,
                    BaseObject* target, RegionInfo* targetRegion, bool knownEmpty)
{
    if (stats.deadTarget > maxSamples) {
        return;
    }
    if (stats.sampleCount < kSampleLimit) {
        stats.holderSamples[stats.sampleCount] = holder;
        stats.targetSamples[stats.sampleCount] = target;
        ++stats.sampleCount;
    }
    RegionInfo* holderRegion =
        holder == nullptr ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    TypeInfo* holderTip = holder == nullptr ? nullptr : holder->GetTypeInfo();
    const char* holderType = (holderTip == nullptr || holderTip->GetName() == nullptr) ? "?" : holderTip->GetName();

    // "live holder" is marked-or-resurrected, and the two have different tracing
    // rules: MarkObject enqueues for a field scan, ResurrectObject paints the
    // resurrect bitmap and DoResurrection follows separately
    // (TracingCollector.cpp:896-922). Which one this holder is decides whether the
    // question is "who marked it without following it" or "who resurrected it".
    // The target's own two bits and the from-page route state are printed for the
    // same reason -- so the next reader does not have to re-derive them from the
    // address.
    // Put the holder on the notraced watch list. It cannot answer for this cycle --
    // the trace already happened -- but these edges repeat with the same addresses,
    // so the next cycle says whether this holder is ever followed at all. That is
    // the split between "painted live without a field scan" and "scanned, but the
    // target did not get marked".
    WatchHolder(holder);
    const unsigned holderMarked = RegionSpace::IsMarkedObject<Generation::Old>(holder) ? 1u : 0u;
    const unsigned holderResurrected = RegionSpace::IsResurrectedObject(holder) ? 1u : 0u;
    const unsigned targetMarkedBit = RegionSpace::IsMarkedObject<Generation::Old>(target) ? 1u : 0u;
    const unsigned targetResurrected = RegionSpace::IsResurrectedObject(target) ? 1u : 0u;
    const unsigned targetForwardedBit = target->IsForwarded() ? 1u : 0u;
    // The bursts seen so far are a contiguous run of array elements naming a
    // contiguous run of objects in ONE from-region, which does not look like a
    // per-element miss. If a whole region's mark face is absent or is bound to a
    // different epoch than the one this cycle wrote through, every mark aimed at
    // it is lost together -- the failure mode the TraceHeap seed-push comment
    // already guards against ("paint after Assemble+PrepareTrace so ClearLiveInfo
    // cannot wipe the bits", WCollector.cpp:2044-2048). These two epochs are what
    // IsKnownEmpty itself compares (RegionInfo.h:2794-2805), so printing them says
    // whether the region had a usable mark face at all.
    const unsigned long long targetViewEpoch =
        targetRegion == nullptr ? 0ULL
                                : static_cast<unsigned long long>(
                                      targetRegion->GetMarkView<Generation::Old>().GetEpoch());
    const unsigned long long targetSnapEpoch =
        targetRegion == nullptr
            ? 0ULL
            : static_cast<unsigned long long>(targetRegion->GetMarkSnapshotEpoch<Generation::Old>());
    LOG(RTLOG_ERROR,
        "[GCV2][markcomplete] DEAD_EDGE point=%s holder=%p holderType=%s holderRegion=%s "
        "holderMarked=%u holderResurrected=%u "
        "field=%p fieldOffset=%zd target=%p targetRegion=%s targetRegionBase=%p "
        "targetMarked=%u targetResurrected=%u targetForwarded=%u targetRoute=%u "
        "targetViewEpoch=%llu targetSnapEpoch=%llu knownEmpty=%u n=%zu",
        point == nullptr ? "?" : point, holder, holderType, RegionKindName(holderRegion), holderMarked,
        holderResurrected, &field, BaseObject::FieldOffset(holder, &field), target, RegionKindName(targetRegion),
        targetRegion == nullptr ? nullptr : reinterpret_cast<void*>(targetRegion->GetRegionStart()), targetMarkedBit,
        targetResurrected, targetForwardedBit,
        targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->GetRouteState()), targetViewEpoch,
        targetSnapEpoch, static_cast<unsigned>(knownEmpty), stats.deadTarget);
    if (targetRegion != nullptr &&
        (targetRegion->IsFromRegion() || targetRegion->IsLoneFromRegion() ||
         targetRegion->IsUnmovableFromRegion())) {
        SurvNodeDiag::ReportOnDeadEdge(holder, &field, target, targetRegion);
    }
}

void AccountInteriorKind(Stats& stats, size_t maxSamples, const char* point, BaseObject* holder, RefField<>& field,
                         BaseObject* target, RegionInfo* region, BaseObject* recovered);

// One edge out of a live old holder.  Mirrors z_verify_old_oop (zVerify.cpp:131-155):
// the target must be marked old, except for the cases ZGC also exempts.
void CheckEdge(Stats& stats, size_t maxSamples, const char* point, BaseObject* holder, RefField<>& field)
{
    BaseObject* target = to_object(field.GetTargetObject());
    if (target == nullptr) {
        return;
    }
    ++stats.edgesSeen;
    if (!Heap::IsHeapAddress(target)) {
        ++stats.targetNonHeap;
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (region == nullptr) {
        ++stats.deadTarget;
        ++stats.deadNoRegion;
        ReportDeadEdge(stats, maxSamples, point, holder, field, target, nullptr, false);
        return;
    }
    // Young is the minor's mark face; ZGC's old verifier allows bad young bits
    // on old-to-young edges (zVerify.cpp:150).
    if (region->IsYoungRegion()) {
        ++stats.targetYoung;
        return;
    }
    // Allocate-black: objects born after this cycle's ClearLiveInfo carry no mark
    // bit and IsKnownEmpty already refuses to call the region empty
    // (RegionInfo.h:2776-2781).  Not a completeness defect.
    if (region->HasMarkStartAllocGap()) {
        ++stats.targetAllocGap;
        return;
    }
    // An interior pointer (introot's RawArray+8) is live iff its base is marked.
    if (!Collector::PlausibleManagedObjectGate("MarkCompleteVerify.target", target)) {
        BaseObject* base = Collector::TryRecoverInteriorBase(target);
        if (base != nullptr && base != target && IsLiveOld(base)) {
            ++stats.targetInteriorBaseMarked;
            return;
        }
        ++stats.deadTarget;
        ++stats.deadInterior;
        AccountInteriorKind(stats, maxSamples, point, holder, field, target, region, base);
        ReportDeadEdge(stats, maxSamples, point, holder, field, target, region, false);
        return;
    }
    if (IsLiveOld(target)) {
        ++stats.targetMarked;
        return;
    }
    // A from-copy that has already been evacuated legitimately carries no
    // this-cycle mark: the live version is the to-copy, and the slot naming the
    // from-address just has not been fixed yet (ref_fix runs phases later). ZGC's
    // verifier resolves the same case with a load barrier before testing the mark
    // (ZVerifyColoredRootClosure, zVerify.cpp:227-230). Resolving the route here
    // would mean calling into the forwarding machinery at a phase that does not
    // expect it, so record the state the header already carries and keep it out of
    // the defect column rather than guessing either way.
    if (target->IsForwarded()) {
        ++stats.targetForwarded;
        return;
    }

    ++stats.deadTarget;
    const bool knownEmpty = region->IsKnownEmpty(region->GetMarkView<Generation::Old>());
    if (knownEmpty) {
        ++stats.deadTargetKnownEmpty;
    }
    const unsigned wouldFree = WouldCsetPredicateFree(region);
    if (wouldFree == 1u) {
        ++stats.deadTargetWouldFree;
    } else if (wouldFree == 0u) {
        ++stats.deadTargetWouldKeep;
    } else {
        ++stats.deadTargetNotConsidered;
    }
    if (region->IsFromRegion() || region->IsLoneFromRegion() || region->IsUnmovableFromRegion()) {
        ++stats.deadInFrom;
    } else if (region->IsGarbageRegion()) {
        ++stats.deadInGarbage;
    } else if (region->IsFreeRegion()) {
        ++stats.deadInFree;
    } else if (region->IsLargeRegion()) {
        ++stats.deadInLarge;
    } else {
        ++stats.deadInOther;
    }
    ReportDeadEdge(stats, maxSamples, point, holder, field, target, region, knownEmpty);
}

// One strong root. ZVerifyColoredRootClosure's guarantee(is_marked_old)
// (zVerify.cpp:225) with verify_after_old_mark: a strong root may not name an
// object old mark left unmarked.
void CheckRoot(Stats& stats, const char* point, const char* kind, void* slot, zaddress_unsafe slotValue)
{
    // Root slots may still carry colour bits until this cycle's root pass heals
    // them (Mutator.cpp:818-827 PlainRootObject), and an unpeeled value fails
    // IsHeapAddress -- which would silently turn every root into "not ours".
    if (is_null(slotValue)) {
        return;
    }
    BaseObject* obj = to_object(safe(uncolor_bits(to_zpointer(raw(slotValue)))));
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    ++stats.rootsSeen;
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr || region->IsYoungRegion() || region->HasMarkStartAllocGap()) {
        return;
    }
    if (!Collector::PlausibleManagedObjectGate("MarkCompleteVerify.root", obj)) {
        // Stack slots legitimately hold interiors (introot's RawArray+8); the base
        // is what has to be live.
        BaseObject* base = Collector::TryRecoverInteriorBase(obj);
        if (base == nullptr || base == obj || IsLiveOld(base)) {
            return;
        }
        obj = base;
    } else if (IsLiveOld(obj)) {
        return;
    }
    ++stats.rootDead;
    LOG(RTLOG_ERROR, "[GCV2][markcomplete] DEAD_ROOT point=%s kind=%s root=%p obj=%p region=%s n=%zu",
        point == nullptr ? "?" : point, kind, slot, obj, RegionKindName(region), stats.rootDead);
}

// ZVerify::roots_strong (zVerify.cpp:365-392) covers every strong root, not just
// the statics. Checking statics alone reads as "roots are clean" while leaving the
// stacks -- the root set most likely to disagree with the mark face -- unlooked at.
void CheckStrongRoots(Stats& stats, const char* point)
{
    RootSlotVisitor staticVisitor = [&stats, point](RootSlot& root) {
        CheckRoot(stats, point, "static", &root, root.LoadPlain());
    };
    Heap::GetHeap().VisitStaticRoots(staticVisitor);

    RootVisitor stackVisitor = [&stats, point](ObjectRef& root) {
        CheckRoot(stats, point, "mutator", &root, root.LoadPlain());
    };
    // Safe under STW: this is the same traversal DumpRoots uses
    // (TracingCollector.cpp:1003-1004), and the world is stopped by RunAtMarkEnd.
    MutatorManager::Instance().VisitAllMutators(
        [&stackVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(stackVisitor); });

    RootVisitor exportVisitor = [&stats, point](ObjectRef& root) {
        CheckRoot(stats, point, "export", &root, root.LoadPlain());
    };
    Heap::GetHeap().VisitAllExportRoots(exportVisitor);
}

// Independent second enumeration, over the managed region lists rather than the
// address range, repeating the same size-walk the product walk uses.  Its job is
// only to say how much of each region that walk could actually reach.
void CensusWalkCoverage(Stats& stats)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.GetRegionManager().VisitAllManagedRegionsForProbe([&stats](RegionInfo* region, const char*) {
        if (region == nullptr || !region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            return;
        }
        ++stats.regionsWalked;
        if (!region->IsSmallRegion()) {
            return;
        }
        uintptr_t pos = region->GetRegionStart();
        const uintptr_t alloc = region->GetRegionAllocPtr();
        while (pos < alloc) {
            BaseObject* obj = from_region_addr(pos);
            if (!Collector::PlausibleManagedObjectGate("MarkCompleteVerify.census", obj)) {
                break;
            }
            const size_t size = RegionSpace::GetAllocSize(*obj);
            if (size == 0) {
                break;
            }
            pos += size;
        }
        if (pos < alloc) {
            ++stats.regionsTruncated;
            stats.bytesUnwalked += static_cast<size_t>(alloc - pos);
        }
    });
}

bool FatalOnFailure()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_MARKCOMPLETE_FATAL", "markcompletefatal");
    return on;
}

// Nested census of deadInterior. Off unless MARKCOMPLETE is already on AND
// MRT_GCV2_MARKCOMPLETE_INTEDGE=1 / token "intedge". Size-walk and GCTib
// lookup only run on the deadInterior arm, so deadFrom is untouched.
bool IntedgeEnabled()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_MARKCOMPLETE_INTEDGE", "intedge");
    return on;
}

bool GctibBitIsRef(GCTib gcTib, size_t wordIndex)
{
    if (gcTib.IsGCTibWord()) {
        ArchUInt gcInfo = gcTib.bitmap.bitmap & (~SIGN_BIT);
        return ((gcInfo >> wordIndex) & 1u) != 0;
    }
    StdGCTib* large = gcTib.gctib;
    if (large == nullptr) {
        return false;
    }
    const size_t byteIndex = wordIndex / 8u;
    if (byteIndex >= large->nBitmapWords) {
        return false;
    }
    const U8 bit = static_cast<U8>(1u << (wordIndex % 8u));
    return (large->bitmapWords[byteIndex] & bit) != 0;
}

// Independent of ForEachRefField: ask the TypeInfo bitmap whether this
// payload word is a reference. ForEachRefField is how the verifier arrived;
// a false here is the SlotNotRef case (non-ref word walked as a ref).
bool HolderFieldIsRef(BaseObject* holder, RefField<>& field)
{
    if (holder == nullptr) {
        return false;
    }
    TypeInfo* tip = holder->GetTypeInfo();
    if (tip == nullptr || !Collector::PlausibleManagedObjectGate("MarkCompleteVerify.intedge.holderTip", holder)) {
        return false;
    }
    const intptr_t off = BaseObject::FieldOffset(holder, &field);
    if (off < 0) {
        return false;
    }
    const size_t uoff = static_cast<size_t>(off);
    if (tip->IsRawArray()) {
        auto* arr = reinterpret_cast<MArray*>(holder);
        TypeInfo* component = arr->GetComponentTypeInfo();
        if (component == nullptr) {
            return false;
        }
        const size_t contentOff = sizeof(MArray);
        if (uoff < contentOff) {
            return false;
        }
        if (component->IsRef()) {
            return ((uoff - contentOff) % sizeof(HeapSlot<>)) == 0;
        }
        if (!component->IsStructType() || !component->HasRefField()) {
            return false;
        }
        // Struct elements have no TypeInfo header. ForEachElementInArray
        // (BaseObject.cpp:109-115) feeds GCTib the element origin, so bit 0 is
        // the first word of the struct, not origin+TYPEINFO_PTR_SIZE.
        const size_t elemSize = component->GetComponentSize();
        if (elemSize == 0) {
            return false;
        }
        const size_t inElem = (uoff - contentOff) % elemSize;
        if ((inElem % sizeof(HeapSlot<>)) != 0) {
            return false;
        }
        return GctibBitIsRef(component->GetGCTib(), inElem / sizeof(HeapSlot<>));
    }
    if (uoff < TYPEINFO_PTR_SIZE) {
        return false;
    }
    if (!tip->HasRefField()) {
        return false;
    }
    return GctibBitIsRef(tip->GetGCTib(), (uoff - TYPEINFO_PTR_SIZE) / sizeof(HeapSlot<>));
}

unsigned EnsureStreamIndex(Stats& stats, RegionInfo* region)
{
    if (region == nullptr) {
        return 1u;
    }
    auto found = stats.streamTruncated.find(region);
    if (found != stats.streamTruncated.end()) {
        return found->second;
    }
    std::vector<std::pair<uintptr_t, size_t>> starts;
    unsigned truncated = 0;
    if (region->IsLargeRegion()) {
        BaseObject* obj = from_region_addr(region->GetRegionStart());
        if (Collector::PlausibleManagedObjectGate("MarkCompleteVerify.intedge.stream", obj)) {
            const size_t sz = RegionSpace::GetAllocSize(*obj);
            if (sz != 0) {
                starts.emplace_back(region->GetRegionStart(), sz);
            } else {
                truncated = 1u;
            }
        } else {
            truncated = 1u;
        }
    } else if (region->IsSmallRegion()) {
        uintptr_t pos = region->GetRegionStart();
        const uintptr_t alloc = region->GetRegionAllocPtr();
        while (pos < alloc) {
            BaseObject* obj = from_region_addr(pos);
            if (!Collector::PlausibleManagedObjectGate("MarkCompleteVerify.intedge.stream", obj)) {
                truncated = 1u;
                break;
            }
            const size_t sz = RegionSpace::GetAllocSize(*obj);
            if (sz == 0) {
                truncated = 1u;
                break;
            }
            starts.emplace_back(pos, sz);
            pos += sz;
        }
        if (pos < alloc) {
            truncated = 1u;
        }
    } else {
        truncated = 1u;
    }
    stats.streamIndex[region] = std::move(starts);
    stats.streamTruncated[region] = truncated;
    return truncated;
}

BaseObject* FindContainingObject(Stats& stats, RegionInfo* region, BaseObject* target)
{
    if (region == nullptr || target == nullptr) {
        return nullptr;
    }
    (void)EnsureStreamIndex(stats, region);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    const auto& starts = stats.streamIndex[region];
    for (const auto& entry : starts) {
        if (addr >= entry.first && addr < entry.first + entry.second) {
            return from_region_addr(entry.first);
        }
    }
    return nullptr;
}

void AccountInteriorKind(Stats& stats, size_t maxSamples, const char* point, BaseObject* holder, RefField<>& field,
                         BaseObject* target, RegionInfo* region, BaseObject* recovered)
{
    if (!IntedgeEnabled()) {
        return;
    }
    const bool slotIsRef = HolderFieldIsRef(holder, field);
    BaseObject* contain = FindContainingObject(stats, region, target);
    const bool inStream = contain != nullptr && contain != target;
    const bool containLive = inStream && IsLiveOld(contain);
    const bool recoverFound = recovered != nullptr && recovered != target;
    const bool recoverLive = recoverFound && IsLiveOld(recovered);
    const bool walkTruncated = EnsureStreamIndex(stats, region) != 0 && !inStream;
    const InteriorEdgeClass::Kind kind =
        InteriorEdgeClass::Classify(slotIsRef, inStream, containLive, recoverFound, recoverLive, walkTruncated);
    switch (kind) {
        case InteriorEdgeClass::Kind::SlotNotRef:
            ++stats.deadIntSlotNotRef;
            break;
        case InteriorEdgeClass::Kind::RecoverFail:
            ++stats.deadIntRecoverFail;
            break;
        case InteriorEdgeClass::Kind::BaseUnmarked:
            ++stats.deadIntBaseUnmarked;
            break;
        case InteriorEdgeClass::Kind::ValueCorrupt:
            ++stats.deadIntValueCorrupt;
            break;
    }
    if (stats.deadInterior > maxSamples) {
        return;
    }
    const unsigned containMarked = contain != nullptr && RegionSpace::IsMarkedObject<Generation::Old>(contain) ? 1u : 0u;
    TypeInfo* containTip = contain == nullptr ? nullptr : contain->GetTypeInfo();
    const char* containType = (containTip == nullptr || containTip->GetName() == nullptr) ? "?" : containTip->GetName();
    const zpointer rawSlot = field.GetFieldValue();
    const uintptr_t containOff =
        contain == nullptr ? 0u : reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(contain);
    LOG(RTLOG_ERROR,
        "[GCV2][markcomplete] DEAD_INTERIOR point=%s kind=%s slotRaw=%p slotIsRef=%u "
        "inStream=%u contain=%p containOff=%zu containType=%s containMarked=%u containLive=%u "
        "recover=%p recoverFound=%u recoverLive=%u walkTruncated=%u "
        "holder=%p fieldOffset=%zd target=%p regionBase=%p n=%zu",
        point == nullptr ? "?" : point, InteriorEdgeClass::KindName(kind),
        reinterpret_cast<void*>(raw(rawSlot)), static_cast<unsigned>(slotIsRef),
        static_cast<unsigned>(inStream), contain, static_cast<size_t>(containOff), containType, containMarked,
        static_cast<unsigned>(containLive), recovered, static_cast<unsigned>(recoverFound),
        static_cast<unsigned>(recoverLive), static_cast<unsigned>(walkTruncated), holder,
        BaseObject::FieldOffset(holder, &field), target,
        region == nullptr ? nullptr : reinterpret_cast<void*>(region->GetRegionStart()), stats.deadInterior);
}

size_t MaxSamples()
{
    return 64;
}

} // namespace

void WatchHolder(const BaseObject* obj)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    TraceWatchSlot& slot = g_traceWatch[TraceWatchSlotOf(addr)];
    uintptr_t expected = slot.addr.load(std::memory_order_acquire);
    if (expected == addr) {
        return;
    }
    if (expected != 0) {
        g_traceCollisions.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (slot.addr.compare_exchange_strong(expected, addr, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        slot.traced.store(0, std::memory_order_relaxed);
        slot.moved.store(0, std::memory_order_relaxed);
        g_traceWatched.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteHolderTrace(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    TraceWatchSlot& slot = g_traceWatch[TraceWatchSlotOf(addr)];
    if (slot.addr.load(std::memory_order_acquire) == addr) {
        slot.traced.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteHolderCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done)
{
    (void)size;
    (void)done;
    if (!Enabled() || fromAddr == nullptr || toAddr == nullptr) {
        return;
    }
    const uintptr_t from = reinterpret_cast<uintptr_t>(fromAddr);
    TraceWatchSlot& slot = g_traceWatch[TraceWatchSlotOf(from)];
    if (slot.addr.load(std::memory_order_acquire) != from) {
        return;
    }
    slot.moved.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t to = reinterpret_cast<uintptr_t>(toAddr);
    TraceWatchSlot& dst = g_traceWatch[TraceWatchSlotOf(to)];
    uintptr_t empty = 0;
    if (dst.addr.compare_exchange_strong(empty, to, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        dst.traced.store(0, std::memory_order_relaxed);
        dst.moved.store(0, std::memory_order_relaxed);
    } else {
        g_traceCollisions.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReportHolderTraces(const char* point)
{
    if (!Enabled()) {
        return;
    }
    const uint64_t watched = g_traceWatched.load(std::memory_order_relaxed);
    LOG(RTLOG_ERROR, "[GCV2][markcomplete][retrace] point=%s watched=%llu collisions=%llu",
        point == nullptr ? "?" : point, static_cast<unsigned long long>(watched),
        static_cast<unsigned long long>(g_traceCollisions.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < kTraceWatchSlots; ++i) {
        const uintptr_t addr = g_traceWatch[i].addr.load(std::memory_order_acquire);
        if (addr == 0) {
            continue;
        }
        const uint64_t traced = g_traceWatch[i].traced.load(std::memory_order_relaxed);
        LOG(RTLOG_ERROR,
            "[GCV2][markcomplete][retrace] holder=%#zx traced=%llu moved=%llu case=%s",
            static_cast<size_t>(addr), static_cast<unsigned long long>(traced),
            static_cast<unsigned long long>(g_traceWatch[i].moved.load(std::memory_order_relaxed)),
            traced == 0 ? "painted-not-followed" : "followed");
    }
}

bool Enabled()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_MARKCOMPLETE", "markcomplete");
    return on;
}

void RunAtMarkEnd(const char* point)
{
    if (!Enabled()) {
        return;
    }
    static std::atomic<size_t> invokeCount{ 0 };
    const size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;

    const uint64_t startNs = TimeUtil::NanoSeconds();
    Stats stats;
    const size_t maxSamples = MaxSamples();

    {
        // ZVerify::objects asserts it runs at a safepoint (zVerify.cpp:473).  The
        // mark face and every region's live count have to be read while nothing
        // mutates them, or a zero here would only mean "we looked at the wrong
        // instant" -- the failure mode the earlier `!is_marked` runs could not rule out.
        ScopedStopTheWorld stw("markcomplete verify", false);
        Heap::GetHeap().ForEachObj(
            [&stats, maxSamples, point](BaseObject* obj) {
                if (obj == nullptr) {
                    return;
                }
                ++stats.objectsScanned;
                RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
                    region->IsYoungRegion()) {
                    return;
                }
                if (!Collector::PlausibleManagedObjectGate("MarkCompleteVerify.holder", obj)) {
                    return;
                }
                // Only holders this cycle's mark called live get to make claims about
                // their targets (ZVerifyObjectClosure::do_object, zVerify.cpp:432-444).
                if (!IsLiveOld(obj)) {
                    return;
                }
                ++stats.liveHolders;
                if (!obj->HasRefField() || obj->IsWeakRef()) {
                    return;
                }
                obj->ForEachRefField([&stats, maxSamples, point, obj](RefField<>& field) {
                    CheckEdge(stats, maxSamples, point, obj, field);
                });
            },
            false);
        CheckStrongRoots(stats, point);
        CensusWalkCoverage(stats);
    }

    stats.costNs = TimeUtil::NanoSeconds() - startNs;

    // Positive-control columns sit next to the defect columns on purpose: a zero in
    // deadTarget is only readable when objectsScanned/liveHolders/edgesSeen are non-zero.
    LOG(RTLOG_ERROR,
        "[GCV2][markcomplete] point=%s invoke=%zu objects=%zu liveHolders=%zu edges=%zu "
        "okMarked=%zu okYoung=%zu okAllocGap=%zu okInteriorBase=%zu okNonHeap=%zu okForwarded=%zu "
        "deadTarget=%zu deadKnownEmpty=%zu deadWouldFree=%zu deadWouldKeep=%zu deadNotConsidered=%zu "
        "deadFrom=%zu deadGarbage=%zu deadFree=%zu "
        "deadLarge=%zu deadOther=%zu deadNoRegion=%zu deadInterior=%zu "
        "roots=%zu deadRoots=%zu regionsWalked=%zu regionsTruncated=%zu bytesUnwalked=%zu costNs=%llu "
        "deadIntSlotNotRef=%zu deadIntRecoverFail=%zu deadIntBaseUnmarked=%zu deadIntValueCorrupt=%zu",
        point == nullptr ? "?" : point, invoke, stats.objectsScanned, stats.liveHolders, stats.edgesSeen,
        stats.targetMarked, stats.targetYoung, stats.targetAllocGap, stats.targetInteriorBaseMarked,
        stats.targetNonHeap, stats.targetForwarded, stats.deadTarget, stats.deadTargetKnownEmpty,
        stats.deadTargetWouldFree, stats.deadTargetWouldKeep, stats.deadTargetNotConsidered, stats.deadInFrom,
        stats.deadInGarbage,
        stats.deadInFree, stats.deadInLarge, stats.deadInOther, stats.deadNoRegion, stats.deadInterior,
        stats.rootsSeen, stats.rootDead, stats.regionsWalked, stats.regionsTruncated, stats.bytesUnwalked,
        static_cast<unsigned long long>(stats.costNs), stats.deadIntSlotNotRef, stats.deadIntRecoverFail,
        stats.deadIntBaseUnmarked, stats.deadIntValueCorrupt);
    SurvNodeDiag::ReportAtMarkEnd(point);
    if (FatalOnFailure() && (stats.deadTarget != 0 || stats.rootDead != 0)) {
        CHECK_DETAIL(false,
                     "MRT_GCV2_MARKCOMPLETE_FATAL: mark is not complete at %s: deadTarget=%zu "
                     "deadKnownEmpty=%zu deadRoots=%zu liveHolders=%zu edges=%zu",
                     point == nullptr ? "?" : point, stats.deadTarget, stats.deadTargetKnownEmpty, stats.rootDead,
                     stats.liveHolders, stats.edgesSeen);
    }
}

} // namespace MarkCompleteVerify
} // namespace MapleRuntime
