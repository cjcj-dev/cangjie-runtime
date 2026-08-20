// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/MarkCompleteVerify.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace MarkCompleteVerify {
namespace {

constexpr size_t kSampleLimit = 32;

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
    // Region shape of the dead target, for triage.
    size_t deadInFrom = 0;
    size_t deadInGarbage = 0;
    size_t deadInFree = 0;
    size_t deadInLarge = 0;
    size_t deadInOther = 0;
    size_t deadNoRegion = 0;
    size_t deadInterior = 0;

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

    LOG(RTLOG_ERROR,
        "[GCV2][markcomplete] DEAD_EDGE point=%s holder=%p holderType=%s holderRegion=%s "
        "field=%p fieldOffset=%zd target=%p targetRegion=%s targetRegionBase=%p knownEmpty=%u n=%zu",
        point == nullptr ? "?" : point, holder, holderType, RegionKindName(holderRegion), &field,
        BaseObject::FieldOffset(holder, &field), target, RegionKindName(targetRegion),
        targetRegion == nullptr ? nullptr : reinterpret_cast<void*>(targetRegion->GetRegionStart()),
        static_cast<unsigned>(knownEmpty), stats.deadTarget);
}

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

size_t MaxSamples()
{
    return 64;
}

} // namespace

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
        "deadTarget=%zu deadKnownEmpty=%zu deadFrom=%zu deadGarbage=%zu deadFree=%zu "
        "deadLarge=%zu deadOther=%zu deadNoRegion=%zu deadInterior=%zu "
        "roots=%zu deadRoots=%zu regionsWalked=%zu regionsTruncated=%zu bytesUnwalked=%zu costNs=%llu",
        point == nullptr ? "?" : point, invoke, stats.objectsScanned, stats.liveHolders, stats.edgesSeen,
        stats.targetMarked, stats.targetYoung, stats.targetAllocGap, stats.targetInteriorBaseMarked,
        stats.targetNonHeap, stats.targetForwarded, stats.deadTarget, stats.deadTargetKnownEmpty, stats.deadInFrom,
        stats.deadInGarbage,
        stats.deadInFree, stats.deadInLarge, stats.deadInOther, stats.deadNoRegion, stats.deadInterior,
        stats.rootsSeen, stats.rootDead, stats.regionsWalked, stats.regionsTruncated, stats.bytesUnwalked,
        static_cast<unsigned long long>(stats.costNs));

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
