// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_INFO_H
#define MRT_REGION_INFO_H

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#endif
#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Base/RwLock.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/ZForwardingLife.h"
#include "Heap/Collector/GcInfos.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "Heap/Collector/Uncommitter.h"
#include "Heap/Allocator/RouteTicket.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/FillerZeroDiag.h"
#include "Heap/Verify/TagReuseProbe.h"
#include "Heap/Verify/MarkWhyProbe.h"
#include "Heap/Verify/SurvNodeDiag.h"
#include "Heap/Allocator/RouteDestHold.h"
#include "Heap/Verify/FromPageDetachCheck.h"
#include "Heap/Allocator/ForwardingTable.h"
#include "Heap/Allocator/MemMap.h"
#include "Heap/Verify/MutatorRelocate.h"
#include "Heap/Verify/M0Correlation.h"
#include "Base/TimeUtils.h"
#include "securec.h"
#ifdef CANGJIE_ASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
class RegionList;
template<typename T>
class BitField {
public:
    // pos: the position where the bit locates. It starts from 0.
    // bitLen: the length that is to be read.
    T GetAtomicValue(size_t pos, size_t bitLen) const
    {
        T value = __atomic_load_n(&fieldVal, __ATOMIC_ACQUIRE);
        T bitMask = FieldMask(pos, bitLen);
        return value & bitMask;
    }
    void SetAtomicValue(size_t pos, size_t bitLen, T newValue)
    {
        do {
            T oldValue = fieldVal;
            T bitMask = FieldMask(pos, bitLen);
            T unchangedBitMask = ~bitMask;
            T newFieldValue = (static_cast<T>(newValue << pos) & bitMask) | (oldValue & unchangedBitMask);
            if (__atomic_compare_exchange_n(&fieldVal, &oldValue, newFieldValue, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                return;
            }
        } while (true);
    }

private:
    static constexpr T FieldMask(size_t pos, size_t bitLen)
    {
        constexpr size_t width = std::numeric_limits<T>::digits;
        const T lowMask = bitLen >= width ? static_cast<T>(~T(0))
                                          : static_cast<T>((T(1) << bitLen) - T(1));
        return static_cast<T>(lowMask << pos);
    }

    T fieldVal;
};
// this class is the metadata of region, it contains all the information needed to manage its corresponding memory.
// Region memory is composed of several Units, described by UnitInfo.
// sizeof(RegionInfo) must be equal to sizeof(UnitInfo). We rely on this fact to calculate region-related address.


/*
    the layout of unitInfo(UI) and unit(U):
    ...|UI(n+2)|UI(n+1)|UI(n)|.........|.........|U(n)|U(n+1)|U(n+2)|...
                       ↑               ↑         ↑
                  RegionInfo  heapStartAddress  Region
    the offset of unit index and unitInfo index is 1
*/
// region info is stored in the metadata of its primary unit (i.e. the first unit).
class RegionInfo {
public:
    using CompactRouteTable = std::unordered_map<size_t, MAddress>;
    using RouteStartTable = std::unordered_map<size_t, uint8_t>;

    enum class RetainedLiveInfoState : uint8_t {
        NEVER_EXAMINED,
        SNAPSHOT_VALID,
        SNAPSHOT_EMPTY,
        // A retained snapshot was published in this snapshot cycle and its
        // carrier is no longer current.  This is derived from the monotonic
        // ever-preserved bit; clear/unbind exits never write this state.
        SNAPSHOT_LOST,
    };

    // holderlive (F2): the only object-level holder-liveness filter we have reads
    // GetRetainedLiveInfoState() at WCollector.cpp:3579 and measured NEVER_EXAMINED for
    // 100% of holders (never=2787/originFound=2787 per minor). NEVER_EXAMINED has three
    // distinct producers and the state word cannot tell them apart:
    //   - nobody ever called Preserve* on this region during its current life,
    //   - Preserve* ran but had no live info to keep (it writes NEVER_EXAMINED itself),
    //   - Preserve* ran and stored a snapshot, then a clear path wiped it
    //     (SNAPSHOT_LOST, which is now distinguishable from NEVER_EXAMINED).
    // These counters name which one happened. Maintained unconditionally (three stores on
    // cold region-lifecycle paths); read only under MRT_GCV2_RETLIVE_PROBE.
    enum RetainedOp : uint8_t {
        RETAINED_OP_NONE = 0,
        RETAINED_OP_PRESERVE_VALID = 1,
        RETAINED_OP_PRESERVE_EMPTY = 2,
        RETAINED_OP_PRESERVE_NEVER = 3,
        RETAINED_OP_CLEAR_CHECKED = 4,   // CheckAndClearLiveInfo (RegionInfo.h:1271)
        RETAINED_OP_CLEAR_ALL = 5,       // ClearLiveInfo (RegionInfo.h:1297)
        RETAINED_OP_CLEAR_RANGE = 6,     // NullLiveInfoFieldsInRange (RegionInfo.h:1327)
        RETAINED_OP_COUNT = 7,
    };

    enum RouteState : uint8_t {
        NORMAL = 0,
        FORWARDABLE,
        ROUTING,
        ROUTED,
        COMPACTED,
        FORWARDED,
    };

    // State and life are one publication. A reader must never validate an old
    // terminal state with a new-life stamp (zForwarding.inline.hpp:226-251).
    static constexpr unsigned ROUTE_STATE_BITS = 3;
    static constexpr uint64_t ROUTE_STATE_MASK = (uint64_t(1) << ROUTE_STATE_BITS) - 1;

    static uint64_t PackRouteState(RouteState state, RegionLifeId life)
    {
        if (UNLIKELY(life > (std::numeric_limits<uint64_t>::max() >> ROUTE_STATE_BITS))) {
            LOG(RTLOG_FATAL,
                "[LIFECLOCK][ROUTE_SNAPSHOT_OVERFLOW] life=%llu; packed route life cannot wrap",
                static_cast<unsigned long long>(life));
            return 0;
        }
        return (life << ROUTE_STATE_BITS) | static_cast<uint64_t>(state);
    }

    static RouteState RouteStateFromSnapshot(uint64_t snapshot)
    {
        return static_cast<RouteState>(snapshot & ROUTE_STATE_MASK);
    }

    static RegionLifeId RouteLifeFromSnapshot(uint64_t snapshot)
    {
        return snapshot >> ROUTE_STATE_BITS;
    }

    static const size_t UNIT_SIZE; // same as system page size

    // regarding a object as a large object when the size is greater than 8 units.
    static const size_t LARGE_OBJECT_DEFAULT_THRESHOLD;

    // release a large object when the size is greater than 4096KB.
    static constexpr size_t LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB;

    bool CompareExchangeRouteState(RouteState expected, RouteState newWord)
    {
        const RegionLifeId life = GetRegionLifeId();
        uint64_t expectedSnapshot = PackRouteState(expected, life);
        const uint64_t newSnapshot = PackRouteState(newWord, life);
        bool success = metadata.routeStateSnapshot.compare_exchange_strong(
            expectedSnapshot, newSnapshot, std::memory_order_acq_rel, std::memory_order_acquire);
        if (success) {
            RegionLifeClock::Publish(RegionLifeClock::Carrier::ROUTE_STATE, life);
        }
        return success;
    }

    RouteState GetRouteState() const;

    void SetRouteState(RouteState state)
    {
        const RegionLifeId life = GetRegionLifeId();
        metadata.routeStateSnapshot.store(PackRouteState(state, life), std::memory_order_release);
        RegionLifeClock::Publish(RegionLifeClock::Carrier::ROUTE_STATE, life);
    }

    RegionLifeId GetRouteStateLifeId() const
    {
        return RouteLifeFromSnapshot(metadata.routeStateSnapshot.load(std::memory_order_acquire));
    }

    // sealcheck: mark face frozen for geometry (M3). Set at RouteRegion ROUTING entry.
    bool IsMarkFaceSealed() const
    {
        return (__atomic_load_n(&metadata.markFaceSealed, std::memory_order_acquire) & MARK_FACE_SEALED_BIT) != 0;
    }
    void SetMarkFaceSealed(bool v)
    {
        if (v) {
            __atomic_fetch_or(&metadata.markFaceSealed, MARK_FACE_SEALED_BIT, __ATOMIC_RELEASE);
        } else {
            __atomic_fetch_and(&metadata.markFaceSealed, static_cast<uint8_t>(~MARK_FACE_SEALED_BIT),
                               __ATOMIC_RELEASE);
        }
    }

    uint64_t GetSnapshotEpoch() const
    {
        return __atomic_load_n(&metadata.snapshotEpoch, std::memory_order_acquire) >> 1;
    }

    uint8_t GetRegionLifeSeq() const
    {
        return static_cast<uint8_t>(__atomic_load_n(&metadata.routeDestHold, __ATOMIC_ACQUIRE) >> 1);
    }

    RegionLifeId GetRegionLifeId() const
    {
        return metadata.regionLifeId.load(std::memory_order_acquire);
    }

    bool IsRouteStateLifeCurrent() const
    {
        const RegionLifeId stamp = GetRouteStateLifeId();
        const RegionLifeId current = GetRegionLifeId();
        (void)RegionLifeClock::Validate(RegionLifeClock::Carrier::ROUTE_STATE, stamp, current);
        return stamp == current;
    }

    template<Generation G>
    uint64_t GetMarkSnapshotEpoch() const
    {
        (void)G;
        return GetSnapshotEpoch();
    }

    template<Generation G>
    MarkView<G> GetMarkView()
    {
        // A major closure visits both young and old regions.  A minor closure is
        // only authoritative for young regions, so minting the inverse binding is
        // rejected at the sole constructor boundary.
        CHECK_DETAIL(G != Generation::Young || IsYoungRegion(),
                     "cannot bind a young mark view to old region %p", this);
        const RegionLifeId life = GetRegionLifeId();
        RegionLifeClock::Publish(RegionLifeClock::Carrier::MARK_SNAPSHOT, life);
        return MarkView<G>(this, GetMarkSnapshotEpoch<G>(), life);
    }

    template<Generation G>
    bool ValidateMarkView(MarkView<G> view) const
    {
        CHECK(view.GetRegion() == this);
        return RegionLifeClock::Validate(RegionLifeClock::Carrier::MARK_SNAPSHOT, view.GetLifeId(),
                                         GetRegionLifeId());
    }

    void BumpSnapshotEpoch()
    {
        uint64_t observed = __atomic_load_n(&metadata.snapshotEpoch, __ATOMIC_ACQUIRE);
        uint64_t next;
        do {
            // One monotonic value carries both generation and publication:
            // even = no first paint, odd = current face published. Advancing
            // always lands on the next even generation and retires publication.
            next = (((observed >> 1) + 1) << 1);
            // Raw zero is reserved for an uninitialized region.  Skip it on
            // the sole tagged-generation wrap so the next first paint still
            // satisfies PublishCurrentMarkFace's non-zero generation check.
            if (next == 0) {
                next = 2;
            }
        } while (!__atomic_compare_exchange_n(&metadata.snapshotEpoch, &observed, next, true,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }

    template<Generation G>
    void BumpMarkSnapshotEpoch()
    {
        (void)G;
        BumpSnapshotEpoch();
    }

    // oneseq: tagged bumps so atexit/milestones show whether epoch advances per-list / per-region.
    // Default off; enable with MRT_GCV2_ONESEQ=1 or MRT_GCV2_DIAG=oneseq.
    static bool OneseqDiagEnabled()
    {
        static const bool enabled = DiagGate::LegacyOrToken("MRT_GCV2_ONESEQ", "oneseq");
        return enabled;
    }

    template<Generation G>
    void BumpSnapshotEpochFromClearLiveInfo()
    {
        if (!OneseqDiagEnabled()) {
            BumpMarkSnapshotEpoch<G>();
            return;
        }
        size_t n;
        if (G == Generation::Young) {
            n = oneseqBumpClearYoung.fetch_add(1, std::memory_order_relaxed) + 1;
        } else {
            n = oneseqBumpClearOld.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        BumpMarkSnapshotEpoch<G>();
        EnsureOneseqAtexit();
        // Milestone dump so timeout-killed ALOT runs still leave a line (atexit may not run).
        if (n == 1 || n == 8 || n == 64 || n == 256 || n == 1024 || (n >= 4096 && (n & (n - 1)) == 0)) {
            ReportOneseqCounts(G == Generation::Young ? "clear_young_milestone" : "clear_old_milestone");
        }
    }
    void BumpSnapshotEpochFromInitRegion()
    {
        if (!OneseqDiagEnabled()) {
            BumpSnapshotEpoch();
            return;
        }
        size_t n = oneseqBumpInitRegion.fetch_add(1, std::memory_order_relaxed) + 1;
        BumpSnapshotEpoch();
        EnsureOneseqAtexit();
        if (n == 1 || n == 64 || n == 1024 || (n >= 4096 && (n & (n - 1)) == 0)) {
            ReportOneseqCounts("init_milestone");
        }
    }
    template<Generation G>
    void BumpSnapshotEpochFromResetAfterForward()
    {
        if (!OneseqDiagEnabled()) {
            BumpMarkSnapshotEpoch<G>();
            return;
        }
        size_t n = oneseqBumpResetAfterForward.fetch_add(1, std::memory_order_relaxed) + 1;
        BumpMarkSnapshotEpoch<G>();
        EnsureOneseqAtexit();
        if (n == 1 || n == 64 || n == 1024 || (n >= 4096 && (n & (n - 1)) == 0)) {
            ReportOneseqCounts("reset_fwd_milestone");
        }
    }

    static void EnsureOneseqAtexit();
    static void ReportOneseqCounts(const char* point);

    bool IsCompacted() { return GetRouteState() == RouteState::COMPACTED; }

    bool IsRoutingState() { return GetRouteState() == RouteState::ROUTING; }

    // enroltime: when does a region actually join the relocation set?
    //
    // OpenJDK installs the whole set before the colour flips: ZRelocationSet::install runs in the
    // concurrent select_relocation_set (zGeneration.cpp:254), and only afterwards does
    // relocate_start -> flip_relocate_start run (:918 -> :922 -> :651).  Because of that ordering,
    // "painted with the current colour, after the flip" is equivalent to "names an object that
    // will not move this cycle", and ZGC's whole colour-epoch argument rests on that equivalence.
    //
    // Ours enrols lazily: RouteRegion drives each region's RouteState out of NORMAL as forwarding
    // reaches it.  If that happens after the flip, the equivalence breaks -- a value painted
    // store-good in between is load-good and later names a from-version, which is exactly the
    // measured FORWARD population (afterFlip=1, slotGood=1, hasTo=1, 20/20) whose targets must have
    // had RouteState == NORMAL when they were painted, since every non-NORMAL target is already
    // caught by the staleness predicate (ROUTEASK: 105 triggers, 100% covered).
    //
    // GCPhase is the cheap witness: PREFORWARD/FORWARD mean the relocate-start flip has run.
    static constexpr bool kEnrolTimeProbe = true;
    static std::atomic<uint64_t>& EnrolBeforeFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }
    static std::atomic<uint64_t>& EnrolAfterFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }
    void NoteEnrolPhase();

    bool TryLockRouting(RouteState curState)
    {
        if (IsRoutingState()) {
            return false;
        }
        const bool locked = CompareExchangeRouteState(curState, RouteState::ROUTING);
        if (kEnrolTimeProbe && locked && curState == RouteState::NORMAL) {
            NoteEnrolPhase();
        }
        return locked;
    }

    // Probe-only: ghost preLiveBytes (product callers must hold a RouteTicket).
    size_t GetPreLiveBytesInGhostRegionForProbe(MAddress address)
    {
        return GetPreLiveBytesInGhostRegion(address);
    }

    RegionInfo()
    {
        metadata.allocPtr = reinterpret_cast<uintptr_t>(nullptr);
        metadata.regionEnd = reinterpret_cast<uintptr_t>(nullptr);
    }
    static inline RegionInfo* NullRegion()
    {
        static RegionInfo nullRegion;
        return &nullRegion;
    }

    LiveInfo* GetLiveInfo()
    {
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return liveInfo;
    }

    LiveInfo* GetLiveInfo() const
    {
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return liveInfo;
    }

    template<Generation G>
    LiveInfo* GetLiveInfoForView(MarkView<G> view) const
    {
        LiveInfo* current = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(current) != LiveInfo::TEMPORARY_PTR && current != nullptr &&
            current->GetMarkFace().epoch.load(std::memory_order_acquire) == view.GetEpoch()) {
            return current;
        }
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from != nullptr && from->owner == static_cast<uint8_t>(G) && from->epoch == view.GetEpoch()) {
            return from->liveInfo;
        }
        return nullptr;
    }

    ZForwarding* GetFromPageCarrier() const
    {
        ZForwarding* carrier = ForwardingTable::GetEntries(GetRegionStart());
        return carrier != nullptr && carrier->page() == this ? carrier : nullptr;
    }

    const ZForwarding::FromPageView* GetFromPageView() const
    {
        return ForwardingTable::GetFromPageView(const_cast<RegionInfo*>(this));
    }

    bool HasFromPageMetadata() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && RegionLifeClock::Validate(RegionLifeClock::Carrier::MARK_SNAPSHOT,
                                                            from->lifeId, GetRegionLifeId());
    }

    // Probe-only compatibility surface. The storage is no longer a second
    // current face; it belongs to the immutable from-page metadata carrier.
    LiveInfo* GetLiveInfo0ForProbe() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? nullptr : from->liveInfo;
    }

    Generation GetRouteMarkGeneration() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetOwnerGeneration() : static_cast<Generation>(from->owner);
    }

    template<Generation G>
    MarkView<G> GetRouteMarkView()
    {
        CHECK_DETAIL(GetRouteMarkGeneration() == G,
                     "route mark generation mismatch region=%p have=%u want=%u", this,
                     static_cast<unsigned>(GetRouteMarkGeneration()), static_cast<unsigned>(G));
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return GetMarkView<G>();
        }
        return MarkView<G>(this, from->epoch, from->lifeId);
    }

    template<Generation G>
    uint64_t GetMarkEpoch(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return 0;
        }
        return liveInfo == nullptr ? 0 : liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire);
    }

    template<Generation G>
    RegionBitmap* GetMarkBitmap(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return nullptr;
        }
        if (liveInfo == nullptr ||
            liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) != view.GetEpoch()) {
            return nullptr;
        }
        RegionBitmap* bitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        return reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR ? nullptr : bitmap;
    }

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, LiveInfo* liveInfo, size_t offset) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        return liveInfo != nullptr && liveInfo->IsSurvivedObject(view, offset);
    }

    bool FromPageAllocatedAfterMarkStart(size_t offset) const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && from->markStartAllocPtr != 0 &&
            GetRegionStart() + offset >= from->markStartAllocPtr;
    }

    bool HasFromPageMarkStartAllocGap() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from != nullptr && from->markStartAllocPtr != 0 &&
            from->topAtStart > from->markStartAllocPtr;
    }

    template<Generation G>
    bool IsFromPageSurvivedObject(MarkView<G> view, size_t offset) const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            return false;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0 || FromPageAllocatedAfterMarkStart(offset);
        }
        return IsSurvivedObject(view, from->liveInfo, offset) || FromPageAllocatedAfterMarkStart(offset);
    }

    bool IsRouteSurvivedObject(size_t offset)
    {
        if (!HasFromPageMetadata()) {
            if (IsYoungRegion()) {
                MarkView<Generation::Young> view = GetMarkView<Generation::Young>();
                return IsSurvivedObject(view, GetLiveInfo(), offset) || AllocatedAfterMarkStart(offset);
            }
            MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
            return IsSurvivedObject(view, GetLiveInfo(), offset) || AllocatedAfterMarkStart(offset);
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            if (!ValidateMarkView(view)) {
                return false;
            }
            return IsFromPageSurvivedObject(view, offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        if (!ValidateMarkView(view)) {
            return false;
        }
        return IsFromPageSurvivedObject(view, offset);
    }

    // Compatibility name for existing relocation callers. Both route and compact
    // now consume the one owner stored in the from-page metadata carrier.
    bool IsOwnerSurvivedObject(size_t offset)
    {
        return IsRouteSurvivedObject(offset);
    }

    // A "greatest survived start at or below offset" scan used to live here.  It was unsound
    // and is deleted rather than bounded: IsOwnerSurvivedObject is a *coverage* predicate, not
    // a start predicate.  MarkBits paints every 8B slot an object covers (the property this
    // header already states at AdmitForRoute below), so scanning down from an offset returns
    // the last covered slot of the preceding object, never that object's start.  Measured:
    // a 96-slot window around one such refusal reported 82 "starts" for ~7 objects, and the
    // address handed back was 40 bytes inside a 48-byte object -- a garbage base that only the
    // fail-closed load kept out of a root slot.  ZGC has no such ambiguity because ZLiveMap
    // carries one bit pair per object *start* and ZPage::object_iterate is _livemap.iterate
    // (zPage.inline.hpp:320-331); a coverage bitmap cannot be read as if it were that.

    bool IsOwnerKnownEmpty()
    {
        return IsRouteKnownEmpty();
    }

    RegionBitmap* GetOwnerMarkBitmap(LiveInfo* face = nullptr)
    {
        return GetRouteMarkBitmap(face);
    }

    bool IsRouteMarkedObject(size_t offset)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            if (IsYoungRegion()) {
                return IsMarkedObject(GetMarkView<Generation::Young>(), offset);
            }
            return IsMarkedObject(GetMarkView<Generation::Old>(), offset);
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            if (!ValidateMarkView(view)) {
                return false;
            }
            if (FromPageAllocatedAfterMarkStart(offset)) {
                return true;
            }
            if (IsLargeRegion()) {
                return from->largeMarked != 0;
            }
            RegionBitmap* bitmap = GetMarkBitmap(view, from->liveInfo);
            return bitmap != nullptr && bitmap->IsMarked(offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (FromPageAllocatedAfterMarkStart(offset)) {
            return true;
        }
        if (IsLargeRegion()) {
            return from->largeMarked != 0;
        }
        RegionBitmap* bitmap = GetMarkBitmap(view, from->liveInfo);
        return bitmap != nullptr && bitmap->IsMarked(offset);
    }

    bool IsRouteMarkedObject(const BaseObject* object)
    {
        return IsRouteMarkedObject(GetAddressOffset(reinterpret_cast<MAddress>(object)));
    }

    bool IsRouteKnownEmpty()
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (from == nullptr) {
            if (IsYoungRegion()) {
                return IsKnownYoungEmpty(GetMarkView<Generation::Young>());
            }
            return IsKnownEmpty(GetMarkView<Generation::Old>());
        }
        if (HasFromPageMarkStartAllocGap()) {
            return false;
        }
        const uint64_t raw = from->liveByteCount;
        if ((raw & LIVE_AUTHORITY_BIT) == 0) {
            return false;
        }
        if (IsLargeRegion()) {
            return from->largeMarked == 0;
        }
        LiveInfo* live = from->liveInfo;
        return live != nullptr && live->GetMarkFace().epoch.load(std::memory_order_acquire) ==
            from->epoch && (raw & LIVE_BYTES_MASK) == 0;
    }

    RegionBitmap* GetRouteMarkBitmap(LiveInfo* face = nullptr)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        LiveInfo* selected = face != nullptr ? face
            : (from != nullptr ? from->liveInfo : GetLiveInfo());
        if (GetRouteMarkGeneration() == Generation::Young) {
            return GetMarkBitmap(GetRouteMarkView<Generation::Young>(), selected);
        }
        return GetMarkBitmap(GetRouteMarkView<Generation::Old>(), selected);
    }

    uint64_t GetRouteMarkEpoch(LiveInfo* face)
    {
        if (GetRouteMarkGeneration() == Generation::Young) {
            return GetMarkEpoch(GetRouteMarkView<Generation::Young>(), face);
        }
        return GetMarkEpoch(GetRouteMarkView<Generation::Old>(), face);
    }

    uint64_t GetRouteMarkSnapshotEpoch() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? GetSnapshotEpoch() : from->epoch;
    }

    size_t RecomputeRouteBitmapLiveBytes(LiveInfo* face)
    {
        if (face == nullptr) {
            return 0;
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            return face->RecomputeBitmapLiveBytes(GetRouteMarkView<Generation::Young>());
        }
        return face->RecomputeBitmapLiveBytes(GetRouteMarkView<Generation::Old>());
    }

    size_t GetRouteBitmapLiveBytes(LiveInfo* face)
    {
        if (face == nullptr) {
            return 0;
        }
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            return face->GetBitmapLiveBytes(view);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        return face->GetBitmapLiveBytes(view);
    }

    // permhit, probe-only: the route's own to-side plan. RouteInfo records a bare start
    // address plus a used-bytes split (LiveInfo.h:246-248) and carries no epoch, so a
    // to-region that was reclaimed and re-taken keeps answering the same geometry. Only
    // the recorded plan, next to the region that lives at that address now, separates
    // "no path ever filled this tip" from "a tip was filled and the memory was reused".
    // Precedent: GetLiveInfo0ForProbe.
    RouteInfo GetRouteInfoForProbe() const
    {
        return IsRouteInfoLifeCurrent() ? metadata.routeInfo : RouteInfo{};
    }

    // installdomain: if PrepareForwardable snapshotted a null liveInfo, GetRoute always
    // rejects. After MarkObject created current liveInfo, bind it as ghost while still
    // FORWARDABLE so the paint is route-visible (pointer-share, same as PrepareForwardable).
    void BindLiveInfo0FromLiveIfNull()
    {
        if (GetLiveInfo0ForProbe() != nullptr) {
            return;
        }
        LiveInfo* live = GetLiveInfo();
        if (live == nullptr) {
            return;
        }
        const uint64_t epoch = live->GetMarkFace().epoch.load(std::memory_order_acquire);
        const RegionLifeId life = GetRegionLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, live, epoch, GetRegionAllocPtr(), metadata.markStartAllocPtr,
                         __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire),
                         static_cast<uint8_t>(GetOwnerGeneration()),
                         static_cast<uint8_t>((IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked != 0) ||
                                              metadata.isResurrected != 0),
                         life),
                     "from-page forwarding carrier missing while binding live face region=%p", this);
    }

    bool IsRetainedLifeCurrent() const
    {
        const RegionLifeId stamp = __atomic_load_n(&metadata.retainedLifeId, __ATOMIC_ACQUIRE);
        const RegionLifeId current = GetRegionLifeId();
        const bool auditAccepts =
            RegionLifeClock::Validate(RegionLifeClock::Carrier::RETAINED_COPY, stamp, current);
        // Validate is audit-only unless enforcement is enabled and therefore
        // deliberately accepts missing/stale stamps in ordinary product runs.
        // Snapshot-state derivation needs the structural answer in every
        // configuration: zero is not a carrier, and only this life is current.
        return stamp != 0 && stamp == current && auditAccepts;
    }

    LiveInfo* GetRetainedLiveInfo() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfo : nullptr;
    }

    RetainedLiveInfoState GetRetainedLiveInfoState() const
    {
        if (metadata.retainedEverPreserved == 0) {
            return RetainedLiveInfoState::NEVER_EXAMINED;
        }
        if (!IsRetainedLifeCurrent()) {
            return RetainedLiveInfoState::SNAPSHOT_LOST;
        }
        return metadata.retainedLiveInfoCoveredUpTo <= GetRegionStart() &&
                GetRegionAllocPtr() <= GetRegionStart()
            ? RetainedLiveInfoState::SNAPSHOT_EMPTY
            : RetainedLiveInfoState::SNAPSHOT_VALID;
    }

    bool HasEverPreservedRetainedLiveInfo() const { return metadata.retainedEverPreserved != 0; }

    uint64_t GetRetainedLiveInfoEpoch() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfoEpoch : 0;
    }

    MAddress GetRetainedLiveInfoCoveredUpTo() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfoCoveredUpTo : 0;
    }

    void StampRetainedSnapshot()
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&metadata.retainedLifeId, life, __ATOMIC_RELEASE);
        RegionLifeClock::Publish(RegionLifeClock::Carrier::RETAINED_COPY, life);
    }

    // holderlive (F2): the retained snapshot has to answer "was this holder live at the last
    // mark" during every minor until the next major re-marks the region. It cannot do that as a
    // borrowed LiveInfo*: LiveInfo lives in a per-tag arena that is recycled one GC cycle later
    // (ForwardDataManager::ClearPreviousForwardData → ReleaseMemory), and UnbindPreviousLiveInfo
    // (DoGarbageCollection, WCollector.cpp:6122 at 7924d28f) drops every borrowed pointer
    // into it at the end of each major.
    // Measured: 100% of remset holders read NEVER_EXAMINED, and for 2113/2115 of them the last
    // thing that touched the snapshot was that unbind ([RETLIVE][why-never] lastOp=clrChecked).
    // So keep our own copy of the bits — regionSize/512 bytes, allocated only for regions that
    // are actually preserved. ZGC keeps the page livemap valid through relocation
    // (zLiveMap.inline.hpp:38-40,86-90); this copy is the equivalent persistent carrier.
    static constexpr bool RetainedOwnCopyEnabled() { return true; }

    // ZGC's page owns its livemap and ZRelocateAddRemsetForFlipPromoted iterates that
    // page-owned map (zRelocate.cpp:1256-1279 / zPage.inline.hpp:320-331). Our LiveInfo
    // faces live in an epoch-recycled arena, so a consumer registered in one phase and
    // run in a later one cannot hold a view token and expect it to still resolve: the
    // face is gone and every liveness query answers false. Such a consumer takes its own
    // copy of the bits at registration instead. Same union as CaptureRetainedMarkWords
    // (ordinary mark | resurrect); false means there was no face to copy.
    template<Generation G>
    bool CopyMarkWordsForView(MarkView<G> view, std::vector<uint64_t>& out)
    {
        out.clear();
        CHECK(view.GetRegion() == this);
        if (IsLargeRegion()) {
            // ZGC large pages hold one object at page start (zPage.inline.hpp:53-58) but
            // still express its liveness through the page livemap (228-240).
            if (metadata.isMarked == 0 && metadata.isResurrected != 1) {
                return false;
            }
            out.assign(1, static_cast<uint64_t>(1));
            return true;
        }
        if (!ValidateMarkView(view)) {
            return false;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace();
        RegionBitmap* mark = markFace.epoch.load(std::memory_order_acquire) == view.GetEpoch()
            ? __atomic_load_n(&markFace.bitmap, std::memory_order_acquire) : nullptr;
        if (reinterpret_cast<MAddress>(mark) == LiveInfo::TEMPORARY_PTR) {
            mark = nullptr;
        }
        RegionBitmap* resurrect = liveInfo->resurrectBitmap;
        size_t markWords = mark == nullptr ? 0 : mark->wordCnt.load(std::memory_order_acquire);
        size_t resurrectWords = resurrect == nullptr ? 0 : resurrect->wordCnt.load(std::memory_order_acquire);
        size_t wordCnt = std::max(markWords, resurrectWords);
        if (wordCnt == 0) {
            return false;
        }
        out.assign(wordCnt, 0);
        for (size_t i = 0; i < wordCnt; ++i) {
            uint64_t bits = 0;
            if (i < markWords) {
                bits |= mark->markWords[i].load(std::memory_order_acquire);
            }
            if (i < resurrectWords) {
                bits |= resurrect->markWords[i].load(std::memory_order_acquire);
            }
            out[i] = bits;
        }
        return true;
    }

    // Same indexing as RegionBitmap::IsMarked / RetainedMarkWordsSay, over a copy the
    // caller owns.
    static bool SnapshotMarkWordsSay(const std::vector<uint64_t>& words, size_t offset)
    {
        size_t bitIdx = 2 * (offset / kMarkedBytesPerBit);
        size_t wordIdx = bitIdx / kBitsPerWord;
        if (wordIdx >= words.size()) {
            return false;
        }
        return (words[wordIdx] & (static_cast<uint64_t>(1) << (bitIdx % kBitsPerWord))) != 0;
    }

    // Copy the page's one ordinary livemap plus resurrection bits into the
    // retained owner. No generation-dependent face union is needed.
    void CaptureRetainedMarkWords(LiveInfo* liveInfo, uint64_t epoch, uint8_t largeMarked)
    {
        FreeRetainedMarkWords();
        if (IsLargeRegion()) {
            // ZGC large pages contain one object at page start (zPage.inline.hpp:53-58),
            // but still represent its liveness with the page livemap (228-240). Mirror
            // our large-region mark/resurrect single bits in retained word bit zero.
            bool marked = largeMarked != 0 || metadata.isResurrected == 1;
            if (!marked) {
                return;
            }
            uint64_t* words = static_cast<uint64_t*>(malloc(sizeof(uint64_t)));
            CHECK(words != nullptr);
            words[0] = 1;
            metadata.retainedMarkWords = words;
            metadata.retainedMarkWordCnt = 1;
            return;
        }
        if (liveInfo == nullptr) {
            return;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace();
        RegionBitmap* mark = markFace.epoch.load(std::memory_order_acquire) == epoch
            ? __atomic_load_n(&markFace.bitmap, std::memory_order_acquire) : nullptr;
        RegionBitmap* resurrect = liveInfo->resurrectBitmap;
        size_t markWords = mark == nullptr ? 0 : mark->wordCnt.load(std::memory_order_acquire);
        size_t resurrectWords = resurrect == nullptr ? 0 : resurrect->wordCnt.load(std::memory_order_acquire);
        size_t wordCnt = std::max(markWords, resurrectWords);
        if (wordCnt == 0) {
            return;
        }
        uint64_t* words = static_cast<uint64_t*>(malloc(wordCnt * sizeof(uint64_t)));
        CHECK(words != nullptr);
        for (size_t i = 0; i < wordCnt; ++i) {
            uint64_t bits = 0;
            if (i < markWords) {
                bits |= mark->markWords[i].load(std::memory_order_acquire);
            }
            if (i < resurrectWords) {
                bits |= resurrect->markWords[i].load(std::memory_order_acquire);
            }
            words[i] = bits;
        }
        metadata.retainedMarkWords = words;
        metadata.retainedMarkWordCnt = static_cast<uint32_t>(wordCnt);
    }

    bool HasRetainedMarkWords() const
    {
        return IsRetainedLifeCurrent() && metadata.retainedMarkWords != nullptr;
    }

    // Same indexing as RegionBitmap::IsMarked.
    bool RetainedMarkWordsSay(size_t offset) const
    {
        if (!IsRetainedLifeCurrent()) {
            return false;
        }
        if (metadata.retainedMarkWords == nullptr) {
            return false;
        }
        size_t bitIdx = 2 * (offset / kMarkedBytesPerBit);
        size_t wordIdx = bitIdx / kBitsPerWord;
        if (wordIdx >= metadata.retainedMarkWordCnt) {
            return false;
        }
        return (metadata.retainedMarkWords[wordIdx] &
                (static_cast<uint64_t>(1) << (bitIdx % kBitsPerWord))) != 0;
    }

    void FreeRetainedMarkWords()
    {
        if (metadata.retainedMarkWords != nullptr) {
            free(metadata.retainedMarkWords);
            metadata.retainedMarkWords = nullptr;
        }
        metadata.retainedMarkWordCnt = 0;
    }

    uint32_t GetRetainedPreserveCount() const
    {
        return __atomic_load_n(&metadata.retainedPreserveCnt, __ATOMIC_ACQUIRE) &
            ~FORWARDING_FACE_RESET_BIT;
    }

    uint32_t GetRetainedClearCount() const { return metadata.retainedClearCnt; }

    uint8_t GetRetainedLastOp() const { return metadata.retainedLastOp; }

    // A Preserve attempt replaces the previous publication.  Keep the
    // monotonic history armed, but invalidate the carrier until this attempt
    // proves that it has a snapshot and publishes it in NoteRetainedPreserve.
    ALWAYS_INLINE void BeginRetainedPreserve()
    {
        __atomic_store_n(&metadata.retainedLifeId, static_cast<RegionLifeId>(0), __ATOMIC_RELEASE);
    }

    void PreserveRetainedLiveInfo()
    {
        BeginRetainedPreserve();
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        // Preserve consumes the large face bit and its live bytes as one
        // snapshot.  They share liveByteCount, so do not split this into two
        // loads that could manufacture a half-published view.
        const bool largeRegion = IsLargeRegion();
        const uint64_t largeState = largeRegion
            ? __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire) : 0;
        const uint64_t largeLiveBytes = largeState & LIVE_BYTES_MASK;
        uint8_t largeMarked = largeRegion
            ? ((largeState & LIVE_FACE_PUBLISHED_BIT) != 0) : metadata.isMarked;
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (metadata.retainedLiveInfo == nullptr && from != nullptr) {
            metadata.retainedLiveInfo = from->liveInfo;
            metadata.retainedLiveInfoEpoch = from->epoch;
            largeMarked = from->largeMarked;
        }
        // A done bit may outlive the face it described.  Suppress the young
        // face while it is still the forwarding face, but keep a later face
        // published by the current snapshot (ZGC's page-face identity rule).
        if (IsYoungRegion() && IsForwardingDone() &&
            (!IsCurrentFacePublished() || IsForwardingFaceCurrent())) {
            metadata.retainedLiveInfo = nullptr;
            largeMarked = 0;
        }
        metadata.retainedLiveInfoCoveredUpTo = GetRegionAllocPtr();
        if (RetainedOwnCopyEnabled()) {
            CaptureRetainedMarkWords(metadata.retainedLiveInfo, metadata.retainedLiveInfoEpoch, largeMarked);
        }
        if (IsLargeRegion()) {
            // A stale byte count without the publication bit belongs to the
            // retired face (for example while ClearLiveInfo is sealing it),
            // never to a current valid carrier.
            if (largeLiveBytes == 0 || largeMarked == 0) {
                NoteRetainedPreserve(GetRegionAllocPtr() <= GetRegionStart());
                return;
            }
            NoteRetainedPreserve(true);
            return;
        }
        if (metadata.retainedLiveInfo != nullptr) {
            NoteRetainedPreserve(true);
            return;
        }
        CHECK(GetLiveByteCount() == 0);
        NoteRetainedPreserve(GetRegionAllocPtr() <= GetRegionStart());
    }

    MAddress GetCensusBoundary() const
    {
        return GetRegionStart() + metadata.censusBoundaryOffset;
    }

    void StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        metadata.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

    void ResetCensusBoundary() { metadata.censusBoundaryOffset = 0; }

    void PreserveRetainedLiveInfoUpTo(MAddress boundary)
    {
        CHECK(boundary >= GetRegionStart() && boundary <= GetRegionAllocPtr());
        if (IsLargeRegion()) {
            PreserveRetainedLiveInfo();
            return;
        }
        BeginRetainedPreserve();
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        uint8_t largeMarked = IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked;
        const ZForwarding::FromPageView* from = GetFromPageView();
        if (metadata.retainedLiveInfo == nullptr && from != nullptr) {
            metadata.retainedLiveInfo = from->liveInfo;
            metadata.retainedLiveInfoEpoch = from->epoch;
            largeMarked = from->largeMarked;
        }
        if (IsYoungRegion() && IsForwardingDone() &&
            (!IsCurrentFacePublished() || IsForwardingFaceCurrent())) {
            metadata.retainedLiveInfo = nullptr;
            largeMarked = 0;
        }
        metadata.retainedLiveInfoCoveredUpTo = boundary;
        if (RetainedOwnCopyEnabled()) {
            CaptureRetainedMarkWords(metadata.retainedLiveInfo, metadata.retainedLiveInfoEpoch, largeMarked);
        }
        if (metadata.retainedLiveInfo == nullptr) {
            // This decision is derived after CaptureRetainedMarkWords has
            // replaced the previous owned carrier.  Once a successful
            // Preserve armed the monotonic bit, carrier absence is LOST on
            // every exit; no clear/unbind exit has to remember to write it.
            CHECK(GetRetainedLiveInfoState() != RetainedLiveInfoState::SNAPSHOT_LOST);
            NoteRetainedPreserve(false);
            return;
        }
        NoteRetainedPreserve(true);
    }

    ALWAYS_INLINE void PreserveRetainedLiveInfo(MAddress coveredUpToOverride)
    {
        if (coveredUpToOverride == GetRegionStart() && GetRegionAllocPtr() != GetRegionStart()) {
            CHECK(GetLiveByteCount() == 0);
            BeginRetainedPreserve();
            metadata.retainedLiveInfo = GetLiveInfo();
            metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            metadata.retainedLiveInfoCoveredUpTo = coveredUpToOverride;
            NoteRetainedPreserve(true);
            return;
        }
        CHECK(coveredUpToOverride == GetRegionAllocPtr());
        PreserveRetainedLiveInfo();
    }

    // holderlive (F2): record the outcome of a Preserve* attempt. Only a
    // successful publication arms the monotonic bit and carrier stamp.
    ALWAYS_INLINE void NoteRetainedPreserve(bool succeeded)
    {
        // The high bits share this word with first-paint publication. Marking
        // may publish from multiple workers, so keep the counter increment in
        // the same atomic modification order instead of losing either flag.
        (void)__atomic_fetch_add(&metadata.retainedPreserveCnt, 1U, __ATOMIC_ACQ_REL);
        if (succeeded) {
            metadata.retainedEverPreserved = 1;
            // Publish last: an acquiring reader that accepts this life also
            // observes the retained pointer/owned words and covered boundary.
            StampRetainedSnapshot();
        }
        switch (GetRetainedLiveInfoState()) {
            case RetainedLiveInfoState::SNAPSHOT_VALID:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_VALID;
                break;
            case RetainedLiveInfoState::SNAPSHOT_EMPTY:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_EMPTY;
                break;
            default:
                metadata.retainedLastOp = RETAINED_OP_PRESERVE_NEVER;
                break;
        }
    }

    // holderlive (F2): a clear only destroys information if there was a snapshot to destroy.
    ALWAYS_INLINE void NoteRetainedClear(RetainedOp op)
    {
        if (GetRetainedLiveInfoState() == RetainedLiveInfoState::NEVER_EXAMINED) {
            return;
        }
        ++metadata.retainedClearCnt;
        metadata.retainedLastOp = static_cast<uint8_t>(op);
    }

    bool IsRetainedSnapshotValid() const
    {
        RetainedLiveInfoState state = GetRetainedLiveInfoState();
        if (state == RetainedLiveInfoState::NEVER_EXAMINED ||
            state == RetainedLiveInfoState::SNAPSHOT_LOST) {
            return false;
        }
        if (!IsRetainedLifeCurrent()) {
            return false;
        }
        // An owned copy is the persistent livemap carrier. Its lifetime is
        // ended explicitly by ClearLiveInfo<Old> or region reinitialization;
        // forwarding's epoch bump only retires the borrowed LiveInfo face.
        if (metadata.retainedMarkWords != nullptr) {
            return true;
        }
        return metadata.retainedLiveInfoEpoch == GetSnapshotEpoch();
    }

    LiveInfo* GetOrAllocLiveInfo()
    {
        do {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (UNLIKELY(reinterpret_cast<uintptr_t>(liveInfo) == LiveInfo::TEMPORARY_PTR)) {
                continue;
            }
            if (LIKELY(liveInfo != nullptr)) {
                return liveInfo;
            }
            LiveInfo* newValue = reinterpret_cast<LiveInfo*>(LiveInfo::TEMPORARY_PTR);
            if (__atomic_compare_exchange_n(&metadata.liveInfo, &liveInfo, newValue, false, std::memory_order_seq_cst,
                                            std::memory_order_relaxed)) {
                LiveInfo* allocatedLiveInfo = ForwardDataManager::GetForwardDataManager().AllocateLiveInfo();
                allocatedLiveInfo->bindedRegion = this;
                allocatedLiveInfo->GetMarkFace().epoch = 0;
                allocatedLiveInfo->GetMarkFace().bitmap = nullptr;
                allocatedLiveInfo->resurrectBitmap = nullptr;
                allocatedLiveInfo->enqueueBitmap = nullptr;
                __atomic_store_n(&metadata.liveInfo, allocatedLiveInfo, std::memory_order_release);
                DLOG(REGION, "region %p@%#zx liveinfo %p", this, GetRegionStart(), metadata.liveInfo);
                return allocatedLiveInfo;
            }
        } while (true);

        return nullptr;
    }

    template<Generation G>
    RegionBitmap* GetMarkBitmap(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return nullptr;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return nullptr;
        }
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace();
        if (face.epoch.load(std::memory_order_acquire) != view.GetEpoch()) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return bitmap;
    }

    template<Generation G>
    RegionBitmap* GetOrAllocMarkBitmap(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        LiveInfo* liveInfo = GetOrAllocLiveInfo();
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace();
        do {
            RegionBitmap* bitmap = __atomic_load_n(&face.bitmap, std::memory_order_acquire);
            if (UNLIKELY(reinterpret_cast<uintptr_t>(bitmap) == LiveInfo::TEMPORARY_PTR)) {
                continue;
            }
            if (LIKELY(bitmap != nullptr)) {
                CHECK(face.epoch.load(std::memory_order_acquire) == view.GetEpoch());
                return bitmap;
            }
            RegionBitmap* newValue = reinterpret_cast<RegionBitmap*>(LiveInfo::TEMPORARY_PTR);
            if (__atomic_compare_exchange_n(&face.bitmap, &bitmap, newValue, false, std::memory_order_seq_cst,
                                            std::memory_order_relaxed)) {
                RegionBitmap* allocated =
                    ForwardDataManager::GetForwardDataManager().AllocateRegionBitmap(GetRegionSize());
                face.epoch.store(view.GetEpoch(), std::memory_order_release);
                __atomic_store_n(&face.bitmap, allocated, std::memory_order_release);
                MarkWhyProbe::NoteMarkBitmapAlloc(this, allocated);
                DLOG(REGION, "region %p@%#zx markbitmap generation=%s bitmap=%p", this, GetRegionStart(),
                     G == Generation::Young ? "young" : "old", allocated);
                return allocated;
            }
        } while (true);

        return nullptr;
    }

    RegionBitmap* GetResurrectBitmap()
    {
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return bitmap;
    }

    RegionBitmap* GetOrAllocResurrectBitmap()
    {
        LiveInfo* liveInfo = GetOrAllocLiveInfo();
        do {
            RegionBitmap* bitmap = __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
            if (UNLIKELY(reinterpret_cast<uintptr_t>(bitmap) == LiveInfo::TEMPORARY_PTR)) {
                continue;
            }
            if (LIKELY(bitmap != nullptr)) {
                return bitmap;
            }
            RegionBitmap* newValue = reinterpret_cast<RegionBitmap*>(LiveInfo::TEMPORARY_PTR);
            if (__atomic_compare_exchange_n(&liveInfo->resurrectBitmap, &bitmap, newValue, false,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                RegionBitmap* allocated =
                    ForwardDataManager::GetForwardDataManager().AllocateRegionBitmap(GetRegionSize());
                __atomic_store_n(&liveInfo->resurrectBitmap, allocated, std::memory_order_release);
                DLOG(REGION, "region %p@%#zx resurrectbitmap %p", this, GetRegionStart(),
                     metadata.liveInfo->resurrectBitmap);
                return allocated;
            }
        } while (true);

        return nullptr;
    }

    RegionBitmap* GetEnqueueBitmap()
    {
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&liveInfo->enqueueBitmap, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return bitmap;
    }

    RegionBitmap* GetOrAllocEnqueueBitmap()
    {
        LiveInfo* liveInfo = GetOrAllocLiveInfo();
        do {
            RegionBitmap* bitmap = __atomic_load_n(&liveInfo->enqueueBitmap, std::memory_order_acquire);
            if (UNLIKELY(reinterpret_cast<uintptr_t>(bitmap) == LiveInfo::TEMPORARY_PTR)) {
                continue;
            }
            if (LIKELY(bitmap != nullptr)) {
                return bitmap;
            }
            RegionBitmap* newValue = reinterpret_cast<RegionBitmap*>(LiveInfo::TEMPORARY_PTR);
            if (__atomic_compare_exchange_n(&liveInfo->enqueueBitmap, &bitmap, newValue, false,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                RegionBitmap* allocated =
                    ForwardDataManager::GetForwardDataManager().AllocateRegionBitmap(GetRegionSize());
                __atomic_store_n(&liveInfo->enqueueBitmap, allocated, std::memory_order_release);
                DLOG(REGION, "region %p@%#zx enqueuebitmap %p", this, GetRegionStart(),
                     metadata.liveInfo->enqueueBitmap);
                return allocated;
            }
        } while (true);

        return nullptr;
    }

    template<Generation G>
    uint8_t GetMarkedRegionFlag(MarkView<G> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return 0;
        }
        // Large pages use one atomic face sequence as both their liveness bit and
        // publication marker. A captured view from an earlier metadata incarnation
        // must not observe a later incarnation's reused bit.
        if (view.GetEpoch() != GetMarkSnapshotEpoch<G>()) {
            const ZForwarding::FromPageView* from = GetFromPageView();
            return from != nullptr && from->owner == static_cast<uint8_t>(G) &&
                from->epoch == view.GetEpoch() ? from->largeMarked : 0;
        }
        if (IsLargeRegion()) {
            return IsCurrentFacePublished() ? 1 : 0;
        }
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1) != 0;
    }

    template<Generation G>
    void SetMarkedRegionFlag(MarkView<G> view, uint8_t flag)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        if (IsLargeRegion()) {
            if (flag != 0) {
                // Setup/STW callers do not always have an object size.  The
                // sized MarkObject path uses TryPublishLargeFace directly so
                // its first paint and byte accounting are one atomic RMW.
                (void)TryPublishLargeFace(view, 0);
                PublishCurrentMarkFace();
            } else {
                ClearCurrentMarkFace();
            }
            return;
        }
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1, flag);
    }

    void ResetMarkBit(MarkView<Generation::Old> view)
    {
        SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_RESET_MARK_BIT, false);
        // CollectLargeGarbage calls this for a live large page immediately
        // after mark. Preserve its one-object livemap before clearing the
        // current face, just as ZPage keeps its live bit through relocation.
        if (IsLargeRegion() && IsSurvivedObject(view, 0)) {
            PreserveRetainedLiveInfo();
        }
        SetMarkedRegionFlag(view, 0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
    }

    Generation GetOwnerGeneration() const
    {
        return IsYoungRegion() ? Generation::Young : Generation::Old;
    }

    template<Generation G>
    bool MarkFaceMatchesOwner() const
    {
        return GetOwnerGeneration() == G;
    }

    static bool PageOwnerVerifyCountOnly()
    {
        static const bool countOnly = []() {
            const char* value = std::getenv("MRT_GCV2_VERIFY_PAGE_OWNER");
            return value != nullptr && std::strcmp(value, "count") == 0;
        }();
        return countOnly;
    }

    static std::atomic<size_t>& PageOwnerMismatchAttempts()
    {
        static std::atomic<size_t> count{0};
        return count;
    }

    static std::atomic<size_t>& PageOwnerMismatchFirstPaints()
    {
        static std::atomic<size_t> count{0};
        return count;
    }

    static void ReportPageOwnerVerifyCounts()
    {
        std::fprintf(stderr, "[GCV2][page-owner] point=atexit mismatch_attempts=%zu first_paints=%zu mode=%s\n",
                     PageOwnerMismatchAttempts().load(std::memory_order_relaxed),
                     PageOwnerMismatchFirstPaints().load(std::memory_order_relaxed),
                     PageOwnerVerifyCountOnly() ? "count" : "assert");
        std::fflush(stderr);
    }

    static void EnsurePageOwnerVerifyAtexit()
    {
        static const bool installed = []() {
            std::atexit([]() { ReportPageOwnerVerifyCounts(); });
            return true;
        }();
        (void)installed;
    }

    template<Generation G>
    void VerifyMarkFaceOwner(const BaseObject* obj, const char* site) const
    {
        EnsurePageOwnerVerifyAtexit();
        if (LIKELY(MarkFaceMatchesOwner<G>())) {
            return;
        }
        const size_t mismatch = PageOwnerMismatchAttempts().fetch_add(1, std::memory_order_relaxed) + 1;
        if (PageOwnerVerifyCountOnly()) {
            if (mismatch <= 8) {
                std::fprintf(stderr,
                             "[GCV2][page-owner] mismatch n=%zu site=%s object=%p region=%p owner=%s face=%s\n",
                             mismatch, site, obj, this,
                             GetOwnerGeneration() == Generation::Young ? "young" : "old",
                             G == Generation::Young ? "young" : "old");
                std::fflush(stderr);
            }
            return;
        }
        CHECK_DETAIL(false, "mark face does not match page owner site=%s object=%p region=%p owner=%s face=%s",
                     site, obj, this, GetOwnerGeneration() == Generation::Young ? "young" : "old",
                     G == Generation::Young ? "young" : "old");
    }

    template<Generation G>
    void NotePageOwnerFirstPaint() const
    {
        if (UNLIKELY(!MarkFaceMatchesOwner<G>())) {
            PageOwnerMismatchFirstPaints().fetch_add(1, std::memory_order_relaxed);
        }
    }

    // livesame / ZGC zMark.inline.hpp + zBitMap.inline.hpp:inc_live — count only on 0→1.
    // MarkBits returns true if already marked; false on first paint. AddLive only then.
    template<Generation G>
    bool MarkObject(MarkView<G> view, const BaseObject* obj)
    {
        CHECK(view.GetRegion() == this);
        if (!PlausibleManagedObjectGate("RegionInfo::MarkObject.unsized", const_cast<BaseObject*>(obj))) {
            // Rejected objects are deliberately reported as already marked: callers must not
            // enqueue/scan them. If the gate ever rejects a real object, its liveness and
            // transitive reference closure are the work lost by this fail-closed branch.
            return true;
        }
        VerifyMarkFaceOwner<G>(obj, "RegionInfo::MarkObject.unsized");
        if (IsLargeRegion()) {
            if (TryPublishLargeFace(view, obj->GetSize())) {
                PublishCurrentMarkFace();
                NotePageOwnerFirstPaint<G>();
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t offset = objAddr - regionStart;
        size_t regionSize = regionEnd - regionStart;
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);

        bool incLive = false;
        bool already = writeBm->MarkBits(offset, objSize, regionSize, incLive);
        if (incLive) {
            AddLiveByteCount(objSize);
            PublishCurrentMarkFace();
            NotePageOwnerFirstPaint<G>();
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized0", G);
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized0", G);
        CHECK(IsMarkedObject(view, offset));
        return already;
    }

    template<Generation G>
    bool MarkObject(MarkView<G> view, const BaseObject* obj, size_t objSize, bool accountLive = true)
    {
        CHECK(view.GetRegion() == this);
        VerifyMarkFaceOwner<G>(obj, "RegionInfo::MarkObject.sized");
        if (IsLargeRegion()) {
            if (TryPublishLargeFace(view, accountLive ? objSize : 0)) {
                PublishCurrentMarkFace();
                NotePageOwnerFirstPaint<G>();
                return false;
            }
            return true;
        }
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t offset = objAddr - regionStart;
        size_t regionSize = regionEnd - regionStart;
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);

        bool incLive = false;
        bool already = writeBm->MarkBits(offset, objSize, regionSize, incLive);
        if (incLive) {
            if (accountLive) {
                AddLiveByteCount(objSize);
            }
            PublishCurrentMarkFace();
            NotePageOwnerFirstPaint<G>();
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized", G);
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized", G);
        CHECK(IsMarkedObject(view, offset));
        return already;
    }

    bool MarkObjectByOwner(const BaseObject* obj)
    {
        if (IsYoungRegion()) {
            return MarkObject(GetMarkView<Generation::Young>(), obj);
        }
        return MarkObject(GetMarkView<Generation::Old>(), obj);
    }

    bool MarkObjectByOwner(const BaseObject* obj, size_t objSize, bool accountLive = true)
    {
        if (IsYoungRegion()) {
            return MarkObject(GetMarkView<Generation::Young>(), obj, objSize, accountLive);
        }
        return MarkObject(GetMarkView<Generation::Old>(), obj, objSize, accountLive);
    }

    bool ResurrectObject(const BaseObject* obj, size_t offset)
    {
        if (IsLargeRegion()) {
            MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
            if (GetMarkedRegionFlag(view) != 0) {
                return true;
            }
            if (metadata.isResurrected != 1) {
                SetResurrectedRegionFlag(1);
                AddLiveByteCount(obj->GetSize());
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t regionSize = regionEnd - regionStart;
        MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
        RegionBitmap* bitmap = GetOrAllocMarkBitmap(view);
        bool incLive = false;
        bool already = bitmap->MarkFinalizableBits(offset, objSize, regionSize, incLive);
        if (incLive) {
            AddLiveByteCount(objSize);
            PublishCurrentMarkFace();
        }
        CHECK(bitmap->IsLive(offset));
        return already;
    }

    bool EnqueueObject(const BaseObject* obj, size_t offset)
    {
        if (IsFreeRegion() || IsGarbageRegion() || GetRegionType() == RegionType::FREE_REGION) {
            return true;
        }
        if (!PlausibleManagedObjectGate("RegionInfo::EnqueueObject", const_cast<BaseObject*>(obj))) {
            // Rejected objects are reported as already enqueued: ShouldEnqueue
            // treats true as "do not SATB-push". Lost work is the SATB entry and
            // the later mark/trace of this address; if the gate ever rejects a
            // real object, that object's SATB-driven liveness is the miss.
            return true;
        }
        if (IsLargeRegion()) {
            if (metadata.isEnqueued != 1) {
                SetEnqueuedRegionFlag(1);
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t regionSize = regionEnd - regionStart;
        CHECK(regionSize > 0);
        RegionBitmap* bitmap = GetOrAllocEnqueueBitmap();
        // enqueue face is not the route geometry face; still report if mark-face sealed.

        bool marked = bitmap->MarkBits(offset, objSize, regionSize);
        CHECK(bitmap->IsMarked(offset));
        return marked;
    }

    bool IsResurrectedObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
            return metadata.isResurrected == 1 && GetMarkedRegionFlag(view) == 0;
        }
        RegionBitmap* bitmap = GetMarkBitmap(GetMarkView<Generation::Old>());
        if (bitmap == nullptr) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return bitmap->IsFinalizable(offset);
    }

    bool IsResurrectedObject(size_t offset)
    {
        if (IsLargeRegion()) {
            MarkView<Generation::Old> view = GetMarkView<Generation::Old>();
            return metadata.isResurrected == 1 && GetMarkedRegionFlag(view) == 0;
        }
        RegionBitmap* bitmap = GetMarkBitmap(GetMarkView<Generation::Old>());
        if (bitmap == nullptr) {
            return false;
        }
        return bitmap->IsFinalizable(offset);
    }

    // markepoch: count reads of a LiveInfo whose markEpoch != region snapshotEpoch.
    // Default product still returns false (same as "no bit"); MRT_GCV2_MARK_EPOCH_ASSERT=1 aborts.
    // Design: ops/design/MARK_EPOCH_DISCIPLINE.md §5 (ZGC zLiveMap.inline.hpp:41-43).
    // Hot path: epoch match is load+cmp only (no atomic). Stale path always counts.
    static std::atomic<size_t> markEpochStaleReadCount;
    static std::atomic<bool> markEpochAtexitInstalled;

    // oneseq: per-region epoch / LIVE_AUTHORITY currency probes (default-off counters and dump).
    static std::atomic<size_t> oneseqBumpClearYoung;
    static std::atomic<size_t> oneseqBumpClearOld;
    static std::atomic<size_t> oneseqBumpInitRegion;
    static std::atomic<size_t> oneseqBumpResetAfterForward;
    static std::atomic<size_t> oneseqIsKnownEmptyCalls;
    static std::atomic<size_t> oneseqAuthBlocksReclaim;   // !auth && emptyByEpoch
    static std::atomic<size_t> oneseqAuthAndEmpty;        // auth && emptyByEpoch (= IsKnownEmpty true)
    static std::atomic<size_t> oneseqAuthNotEmpty;        // auth && !emptyByEpoch
    static std::atomic<size_t> oneseqNoAuthNotEmpty;      // !auth && !emptyByEpoch
    static std::atomic<bool> oneseqAtexitInstalled;
    // cjpmnull2: ZGC empty = this-cycle marked ∧ live==0. Epoch mismatch / no face
    // is "not marked this cycle", not empty (zPage.inline.hpp:223-225).
    static std::atomic<size_t> ikeTrueEmpty;
    static std::atomic<size_t> ikeConservativeKeep;
    static std::atomic<size_t> ikeConservativeKeepBytes;
    static std::atomic<size_t> ikeNullFaceKeep;
    static std::atomic<size_t> ikeEpochKeep;
    static std::atomic<bool> ikeAtexitInstalled;

    static bool MarkEpochAssertEnabled()
    {
        return false;
    }

    static void ReportMarkEpochCounts(const char* point)
    {
        const size_t stale = markEpochStaleReadCount.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[GCV2][mark-epoch] point=%s stale_read=%zu env_assert=%d\n",
                     point != nullptr ? point : "?", stale, MarkEpochAssertEnabled() ? 1 : 0);
        std::fflush(stderr);
    }

    static void EnsureMarkEpochAtexit()
    {
        bool expected = false;
        if (markEpochAtexitInstalled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::atexit([]() { ReportMarkEpochCounts("atexit"); });
        }
    }

    // Returns false if face is stale (counts as unmarked). true ⇒ epoch matches; caller checks bits.
    template<Generation G>
    bool NoteMarkEpochOnRead(MarkView<G> view, LiveInfo* liveInfo)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (liveInfo == nullptr) {
            return false;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace();
        RegionBitmap* bitmap = __atomic_load_n(&markFace.bitmap, std::memory_order_acquire);
        // Absence is ordinary "unmarked", not a stale-livemap read.
        if (bitmap == nullptr || reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        const uint64_t face = markFace.epoch.load(std::memory_order_acquire);
        const uint64_t now = GetMarkSnapshotEpoch<G>();
        const ZForwarding::FromPageView* from = GetFromPageView();
        const bool currentOrFrom = view.GetEpoch() == now ||
            (from != nullptr && from->owner == static_cast<uint8_t>(G) && from->epoch == view.GetEpoch());
        if (currentOrFrom && face == view.GetEpoch()) {
            return true;
        }
        EnsureMarkEpochAtexit();
        size_t n = markEpochStaleReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            LOG(RTLOG_ERROR,
                "[GCV2][mark-epoch] stale_read generation=%s region=%p "
                "viewEpoch=%llu faceEpoch=%llu regionEpoch=%llu n=%zu",
                G == Generation::Young ? "young" : "old", this,
                static_cast<unsigned long long>(view.GetEpoch()), static_cast<unsigned long long>(face),
                static_cast<unsigned long long>(now), n);
        }
        return false;
    }

    template<Generation G>
    bool IsMarkedObject(MarkView<G> view, const BaseObject* obj)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        // markepoch §5: stale face ⇒ unmarked (ZGC is_marked false before bit test).
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
    bool IsMarkedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1;
        }
        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (view.GetEpoch() == GetMarkSnapshotEpoch<G>() && AllocatedAfterMarkStart(offset)) {
            return true;
        }
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1 ||
                (G == Generation::Old && metadata.isResurrected == 1);
        }

        LiveInfo* liveInfo = GetLiveInfoForView(view);
        if (liveInfo == nullptr) {
            return false;
        }
        if (NoteMarkEpochOnRead(view, liveInfo)) {
            RegionBitmap* markBitmap =
                __atomic_load_n(&liveInfo->GetMarkFace().bitmap, std::memory_order_acquire);
            if (markBitmap != nullptr && reinterpret_cast<MAddress>(markBitmap) != LiveInfo::TEMPORARY_PTR &&
                markBitmap->IsLive(offset)) {
                return true;
            }
        }
        if (G == Generation::Old) {
            RegionBitmap* resurrectBitmap =
                __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
            if (resurrectBitmap != nullptr &&
                reinterpret_cast<MAddress>(resurrectBitmap) != LiveInfo::TEMPORARY_PTR &&
                resurrectBitmap->IsMarked(offset)) {
                return true;
            }
        }
        return false;
    }

    bool IsEnqueuedObject(size_t offset)
    {
        RegionBitmap* enqueBitmap = GetEnqueueBitmap();
        if (enqueBitmap == nullptr) {
            return false;
        }
        return enqueBitmap->IsMarked(offset);
    }

    ALWAYS_INLINE size_t GetAddressOffset(MAddress address)
    {
        DCHECK(GetRegionStart() <= address);
        return (address - GetRegionStart());
    }

    enum class UnitRole : uint8_t {
        // for the head unit
        FREE_UNITS = 0,
        SMALL_SIZED_UNITS,
        LARGE_SIZED_UNITS,

        SUBORDINATE_UNIT,
    };

    // region is and must be one of following types during its whole lifecycle.
    // one-to-one mapping to region-lists.

    enum class RegionType : uint8_t {
        FREE_REGION,

        THREAD_LOCAL_REGION,
        RECENT_FULL_REGION,
        FROM_REGION,
        LONE_FROM_REGION,
        UNMOVABLE_FROM_REGION,
        TO_REGION,

        // pinned object will not be forwarded by concurrent copying gc.
        FULL_PINNED_REGION,
        RECENT_PINNED_REGION,

        // region for raw-pointer objects which are exposed to runtime thus can not be moved by any gc.
        // raw-pointer region becomes pinned region when none of its member objects are used as raw pointer.
        RAW_POINTER_PINNED_REGION,

        // allocation context is able and responsible to determine whether it is safe to be collected.
        // There are two kind of region, and the type depends on the allocation size.
        TL_RAW_POINTER_REGION,
        TL_LARGE_RAW_POINTER_REGION,

        LARGE_REGION,
        RECENT_LARGE_REGION,

        GARBAGE_REGION,
    };

    static void Initialize(size_t nUnit, uintptr_t heapAddress, MemMap* memoryOwner = nullptr)
    {
        UnitInfo::totalUnitCount = nUnit;
        UnitInfo::heapStartAddress = heapAddress;
        UnitInfo::memoryOwner = memoryOwner;
        // gatehot: UNIT_SIZE is page size (power of two). ctz → shift for GetUnitIdxAt.
        CHECK(UNIT_SIZE != 0 && (UNIT_SIZE & (UNIT_SIZE - 1)) == 0);
        UnitInfo::unitSizeShift = static_cast<size_t>(__builtin_ctzll(static_cast<unsigned long long>(UNIT_SIZE)));
        // routedest: per-unit metadata is per-page metadata, so any growth here is a
        // percentage of the whole heap. Nobody had measured it; report it once so the cost
        // of routeDestHold (one byte, expected to land in existing padding) is a number
        // rather than an assumption. Paired with the static_assert below.
        // routedest: per-unit metadata is per-page metadata, so growth here is a percentage
        // of the whole heap. Measured on x86_64 at main 0626ab83: 192 bytes without
        // routeDestHold, and 192 with it at its current placement — the flag is free.
        // Pinned so that a future field addition has to be a deliberate edit rather than a
        // silent heap-wide cost, and so that anyone who moves routeDestHold "somewhere more
        // readable" finds out immediately.
        // genface: the independent young mark epoch costs 8 bytes per 4 KiB
        // unit (0.195% of heap capacity); both bitmap faces remain lazy.
        // markwater: the current mark-start pointer is page-owned; the copied
        // from-page watermark is carried by ZForwarding.
        // lifeclock: independent 64-bit region identity plus the five region-local
        // Moving the old top/livemap view to ZForwarding removes it from every
        // reusable UnitInfo; pin the resulting heap-wide metadata cost.
        static_assert(sizeof(UnitInfo) == 240, "per-unit metadata size changed; it is per-page, so price it");
    }

    static RegionInfo* GetRegionInfo(uint32_t idx)
    {
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfo(idx);
        if (LoadUnitRole(unit) == UnitRole::SUBORDINATE_UNIT) {
            return unit->GetMetadata().ownerRegion;
        }
        return reinterpret_cast<RegionInfo*>(unit);
    }

    // Safely query a heap address whose unit may no longer have a live owning region.
    ALWAYS_INLINE static RegionInfo* TryGetRegionInfoAt(uintptr_t allocAddr)
    {
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfoAt(allocAddr);
        if (LoadUnitRole(unit) == UnitRole::SUBORDINATE_UNIT) {
            return unit->GetMetadata().ownerRegion;
        }
        return reinterpret_cast<RegionInfo*>(unit);
    }

    // The caller must know that allocAddr resolves to an extant region owner.
    static RegionInfo* GetRegionInfoAt(uintptr_t allocAddr)
    {
        RegionInfo* region = TryGetRegionInfoAt(allocAddr);
        CHECK_DETAIL(region != nullptr, "heap address %#zx has no owning region", allocAddr);
        return region;
    }

    static bool InGhostFromRegion(BaseObject* obj)
    {
        return GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj)) != nullptr;
    }

    static RegionInfo* GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfoAt(allocAddr);
        if (unit->GetMetadata().regionStateBitField.GetAtomicValue(
                RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) == 0) {
            return nullptr;
        }
        RegionInfo* region = LoadUnitRole0(unit) == UnitRole::SUBORDINATE_UNIT
            ? unit->GetMetadata().ownerRegion0 : reinterpret_cast<RegionInfo*>(unit);
        if (region == nullptr ||
            !RegionLifeClock::Validate(RegionLifeClock::Carrier::GHOST,
                                       __atomic_load_n(&unit->GetMetadata().ghostLifeId, __ATOMIC_ACQUIRE),
                                       region->GetRegionLifeId())) {
            return nullptr;
        }
#if defined(MRT_GC_UNIT_TESTS)
        RunGhostLookupTestHook(region);
#endif
        return region;
    }

#if defined(MRT_GC_UNIT_TESTS)
    using GhostLookupTestHook = void (*)(RegionInfo*);
    MRT_EXPORT static void SetGhostLookupTestHook(GhostLookupTestHook hook);
    MRT_EXPORT static size_t GhostLookupTestHookCalls();

    using RouteStateReadTestHook = void (*)(RegionInfo*);
    MRT_EXPORT static void SetRouteStateReadTestHook(RouteStateReadTestHook hook);
    MRT_EXPORT static size_t RouteStateReadTestHookCalls();
#endif

    static void InitFreeRegion(size_t unitIdx, size_t nUnit)
    {
        RegionInfo* region = reinterpret_cast<RegionInfo*>(RegionInfo::UnitInfo::GetUnitInfo(unitIdx));
        region->InitRegionInfo(nUnit, UnitRole::FREE_UNITS);
    }

    static RegionInfo* InitRegion(size_t unitIdx, size_t nUnit, RegionInfo::UnitRole uclass)
    {
        RegionInfo* region = reinterpret_cast<RegionInfo*>(RegionInfo::UnitInfo::GetUnitInfo(unitIdx));
        region->InitRegion(nUnit, uclass);
        return region;
    }

    static RegionInfo* InitRegionAt(uintptr_t addr, size_t nUnit, RegionInfo::UnitRole uclass)
    {
        size_t idx = RegionInfo::UnitInfo::GetUnitIdxAt(addr);
        return InitRegion(idx, nUnit, uclass);
    }

    static MAddress GetUnitAddress(size_t unitIdx) { return UnitInfo::GetUnitAddress(unitIdx); }

    static void WaitCopiedBeforePayloadWipe(RegionInfo* region, const char* site)
    {
        if (region == nullptr) {
            return;
        }
        const int32_t inflight = region->CopyInflight();
        const unsigned admission = static_cast<unsigned>(region->CopyAdmission());
        const int32_t fwdRef = region->ForwardingRefCount();
        const unsigned fwdClaimed = static_cast<unsigned>(region->ForwardingClaimed());
        const unsigned route = static_cast<unsigned>(region->GetRouteState());
        const unsigned long long life = static_cast<unsigned long long>(region->GetRegionLifeId());
        DLOG(REGION,
             "[GCV2][lockstate] payload-drain site=%s copyAdmission=%u copyCount=%d fwdRef=%d "
             "fwdClaimed=%u route=%u life=%llu region=%p",
             site != nullptr ? site : "?", admission, inflight, fwdRef, fwdClaimed, route, life,
             static_cast<void*>(region));
        if (inflight != 0) {
            std::fprintf(stderr,
                         "[GCV2][lockstate] ZERO_UNDER_COPY site=%s copyAdmission=%u copyCount=%d "
                         "fwdRef=%d fwdClaimed=%u route=%u life=%llu region=%p start=%#zx "
                         "regionType=%u unitRole=%u copyWait=1\n",
                         site != nullptr ? site : "?", admission, inflight, fwdRef, fwdClaimed, route, life,
                         static_cast<void*>(region), static_cast<size_t>(region->GetRegionStart()),
                         static_cast<unsigned>(region->GetRegionType()),
                         static_cast<unsigned>(region->GetUnitRole()));
            std::fflush(stderr);
        }
        // Even an OPEN gate with count 0 must be sealed. A zero snapshot does
        // not prevent a copier from entering after this read.
        region->WaitCopiedInflight();
    }

    static void ClearUnits(size_t idx, size_t cnt,
                           FillerZeroDiag::Site site = FillerZeroDiag::Site::CLEAR_UNITS)
    {
        uintptr_t unitAddress = RegionInfo::GetUnitAddress(idx);
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        RegionInfo* wipeRegion = RegionInfo::TryGetRegionInfoAt(unitAddress);
        WaitCopiedBeforePayloadWipe(wipeRegion, "ClearUnits");
        CHECK_DETAIL(FromPageDetach::FromPageDetachCheck(wipeRegion,
                                                        FromPageDetach::Site::CLEAR_UNITS),
                     "CJRT_FROM_REUSE_GATE bypass reached ClearUnits idx=%zu units=%zu", idx, cnt);
        DLOG(REGION, "clear dirty units[%zu+%zu, %zu) @[%#zx+%zu, %#zx)", idx, cnt, idx + cnt, unitAddress, size,
             RegionInfo::GetUnitAddress(idx + cnt));
        // gcfwdfix: ring of zeroed ranges for WAS_LIVE_BEFORE_CLEAR (MRT_GCV2_TRACE_CLEAR=1).
        TraceClear::NoteRange(static_cast<MAddress>(unitAddress), size, "clear_units", nullptr, 0);

        FillerZeroDiag::Note(site, unitAddress, size);
        MapleRuntime::MemorySet(unitAddress, size, 0, size);
    }

    static void CommitUnits(size_t idx, size_t cnt)
    {
        void* unitAddress = reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx));
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        const size_t committed = UnitInfo::memoryOwner == nullptr ? 0 :
                                 UnitInfo::memoryOwner->CommitMemory(unitAddress, size);
        if (committed != size && committed != 0 && UnitInfo::memoryOwner != nullptr) {
            const size_t cleaned = UnitInfo::memoryOwner->ReleaseMemory(unitAddress, committed);
            CHECK_DETAIL(cleaned == committed,
                         "partial commit cleanup failed idx=%zu units=%zu committed=%zu cleaned=%zu", idx, cnt,
                         committed, cleaned);
        }
        CHECK_DETAIL(committed == size,
                     "commit outside heap reservation idx=%zu units=%zu", idx, cnt);
    }

    static void ReleaseUnits(size_t idx, size_t cnt)
    {
        const size_t released = ReleaseUnitsPartial(idx, cnt);
        CHECK_DETAIL(released == cnt * RegionInfo::UNIT_SIZE,
                     "release outside heap reservation idx=%zu units=%zu released=%zu", idx, cnt, released);
    }

    static size_t ReleaseUnitsPartial(size_t idx, size_t cnt)
    {
#if defined(MRT_GC_UNIT_TESTS)
        if (Uncommitter::CutReleaseBackend()) {
            return 0;
        }
#endif
        void* unitAddress = reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx));
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        RegionInfo* wipeRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(unitAddress));
        WaitCopiedBeforePayloadWipe(wipeRegion, "ReleaseUnits");
        CHECK_DETAIL(FromPageDetach::FromPageDetachCheck(wipeRegion,
                         FromPageDetach::Site::RELEASE_UNITS),
                     "CJRT_FROM_REUSE_GATE bypass reached ReleaseUnits idx=%zu units=%zu", idx, cnt);
        DLOG(REGION, "release physical memory for units [%zu+%zu, %zu) @[%p+%zu, 0x%zx)", idx, cnt, idx + cnt,
             unitAddress, size, RegionInfo::GetUnitAddress(idx + cnt));
        const size_t released = UnitInfo::memoryOwner == nullptr ? 0 :
                                UnitInfo::memoryOwner->ReleaseMemory(unitAddress, size);
#ifdef CANGJIE_ASAN_SUPPORT
        if (released != 0) {
            Sanitizer::OnHeapMadvise(unitAddress, released);
        }
#endif
        return released;
    }

    BaseObject* GetFirstObject() const { return from_region_addr(GetRegionStart()); }

    bool IsEmpty() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionAllocPtr() == GetRegionStart();
    }

    size_t GetRegionSize() const
    {
        MAddress regionStart = GetRegionStart();
        DCHECK(metadata.regionEnd > regionStart);
        return metadata.regionEnd - regionStart;
    }

    // Read-only, defensive extent for the phase-1 detach census. InitRegionInfo
    // calls the census before metadata.regionEnd is installed on a never-used
    // unit, so that case is one unit rather than an underflowed stale extent.
    size_t GetRegionSizeForDetachCheck() const
    {
        const MAddress start = GetRegionStart();
        const MAddress end = metadata.regionEnd;
        const MAddress heapEnd = UnitInfo::heapStartAddress + UnitInfo::totalUnitCount * UNIT_SIZE;
        return end > start && end <= heapEnd ? end - start : UNIT_SIZE;
    }

    size_t GetUnitCount() const { return GetRegionSize() / UNIT_SIZE; }

    size_t GetGhostRegionSize() const
    {
        // The old extent follows the forwarding incarnation. If no carrier is
        // installed (idle/test setup), the only valid extent is the page's
        // current own size.
        ZForwarding* carrier = GetFromPageCarrier();
        return carrier == nullptr ? GetRegionSize() : carrier->size();
    }

    size_t GetGhostRegionUnitCount() const { return GetGhostRegionSize() / UNIT_SIZE; }

    size_t GetAvailableSize() const
    {
        MRT_ASSERT(IsSmallRegion(), "wrong region type");
        return GetRegionEnd() - GetRegionAllocPtr();
    }

    size_t GetRegionAllocatedSize() const { return GetRegionAllocPtr() - GetRegionStart(); }

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpRegionInfo(LogType type) const;
    const char* GetTypeName() const;
#endif

    void VisitAllObjects(const std::function<void(BaseObject*)>&& func);
    bool VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func);

    // After-copy Exempt parks FORWARDED residuals (zRelocate.cpp:1041-1047).
    // CSet empty-select still needs those headers; strip only at the next install,
    // after the table is retired (zRelocationSet.cpp:91-96). A leftover FORWARDED
    // with no table entry makes ForwardObjectImpl return PlanRoute's uncopied dest
    // (si_addr=0x8 / near-golden drift). Does not touch LOCKED (live copier).
    void ClearRelocationResiduals();

    // reset so that this region can be reused for allocation
    void InitFreeUnits()
    {
        CHECK_DETAIL(FromPageDetach::FromPageDetachCheck(this, FromPageDetach::Site::INIT_FREE_UNITS),
                     "CJRT_FROM_REUSE_GATE bypass reached InitFreeUnits region=%p", this);
        FromPageDetach::ReusePermitScope permit;
        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; ++i) {
            array[i].ToFreeRegion();
        }
    }

    void SetRouteInfo(uintptr_t to1, uint32_t to1used = 0, uint32_t to2 = RouteInfo::INVALID_VALUE)
    {
        const RegionLifeId life = GetRegionLifeId();
        metadata.routeInfo.SetRouteInfo(to1, to1used, to2, life);
        RegionLifeClock::Publish(RegionLifeClock::Carrier::ROUTE_INFO, life);
    }

    bool IsRouteInfoLifeCurrent() const
    {
        return RegionLifeClock::Validate(RegionLifeClock::Carrier::ROUTE_INFO,
                                         metadata.routeInfo.GetLifeId(), GetRegionLifeId());
    }

    // Sole mint of RouteTicket. Coverage bits paint every 8B slot of an object,
    // so they cannot prove an exact start. Terminal states consume only exact
    // compact keys, forwarding receipts, or the frozen start set.
    // Anchor: ops/design/ROUTE_DOMAIN.md §2; former guard RegionInfo.h GetRoute.
    ATTR_WARN_UNUSED OptionalRouteTicket AdmitForRoute(BaseObject* fromObj)
    {
        if (fromObj == nullptr) {
            return OptionalRouteTicket();
        }
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        if (fromAddress < GetRegionStart() || fromAddress >= GetRegionEnd() ||
            (fromAddress & (kMarkedBytesPerBit - 1)) != 0) {
            return OptionalRouteTicket();
        }
        size_t offset = GetAddressOffset(fromAddress);
        const RouteState routeState = GetRouteState();
        if (routeState != RouteState::ROUTED && routeState != RouteState::COMPACTED &&
            routeState != RouteState::FORWARDED) {
            return OptionalRouteTicket();
        }

        const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(fromAddress);
        if (lookup.to != 0 && lookup.answer == ForwardingTable::ToAnswer::ArmedHit) {
            return OptionalRouteTicket(fromObj);
        }
        if (lookup.answer == ForwardingTable::ToAnswer::Unavailable) {
            return OptionalRouteTicket();
        }

        CompactRouteTable* compact = LoadCompactRouteTable();
        if (routeState == RouteState::COMPACTED) {
            if (compact == nullptr) {
                return OptionalRouteTicket();
            }
            return compact->find(offset) == compact->end()
                ? OptionalRouteTicket() : OptionalRouteTicket(fromObj);
        }

        const RouteStartTable* starts = LoadRouteStartTable();
        if (starts == nullptr || starts->find(offset) == starts->end()) {
            return OptionalRouteTicket();
        }
        return OptionalRouteTicket(fromObj);
    }

    // Geometric derive; domain is guaranteed by RouteTicket. No survivor re-check.
    // Anchor: LiveInfo.h:230-245; LiveInfo.cpp:15-24; ops/design/ROUTE_DOMAIN.md §2.
    // Compacted: dest is the dense pack slot recorded by CompactRegion, not prefix-sum.
    BaseObject* GetRoute(RouteTicket t)
    {
        if (!IsRouteStateLifeCurrent()) {
            LOG(RTLOG_FATAL,
                "[LIFECLOCK][MUTATOR_STALE_ROUTE_STATE] region=%p current=%llu stamp=%llu",
                this, static_cast<unsigned long long>(GetRegionLifeId()),
                static_cast<unsigned long long>(GetRouteStateLifeId()));
        }
        BaseObject* fromObj = t.From();
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        const ForwardingTable::LookupResult lookup = ForwardingTable::LookupTo(fromAddress);
        if (lookup.to != 0 && lookup.answer == ForwardingTable::ToAnswer::ArmedHit) {
            return from_region_addr(lookup.to);
        }
        CompactRouteTable* compactRouteTable = LoadCompactRouteTable();
        if (compactRouteTable != nullptr) {
            BaseObject* packed = LookupCompactRoute(GetAddressOffset(fromAddress), compactRouteTable);
            // Exact miss is terminal. Identity requires an explicit from→from receipt.
            return packed;
        }
        // FreeCompactRouteTable publishes NORMAL before detaching a compact table. An
        // already-admitted reader that loses the detach race must soft-miss rather than
        // reinterpret a compact destination as prefix-sum geometry.
        RouteState routeState = GetRouteState();
        if (routeState != RouteState::ROUTED && routeState != RouteState::FORWARDED) {
            return nullptr;
        }
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        if (!IsRouteInfoLifeCurrent()) {
            LOG(RTLOG_FATAL,
                "[LIFECLOCK][MUTATOR_STALE_ROUTE_INFO] region=%p current=%llu stamp=%llu",
                this, static_cast<unsigned long long>(GetRegionLifeId()),
                static_cast<unsigned long long>(metadata.routeInfo.GetLifeId()));
        }
        MAddress toAddr = metadata.routeInfo.GetRoute(preLiveBytes);
        // routedom: observe mark-domain at geometric GetRoute call site (default off).

        return from_region_addr(toAddr);
    }

    void FreeCompactRouteTable()
    {
        CompactRouteTable* table = static_cast<CompactRouteTable*>(
            __atomic_exchange_n(&metadata.compactRouteTable, static_cast<void*>(nullptr), __ATOMIC_ACQ_REL));
        if (table != nullptr) {
            RetireCompactRouteTable(table);
        }
    }

    void EnsureCompactRouteTable()
    {
        if (LoadCompactRouteTable() == nullptr) {
            CompactRouteTable* table = new CompactRouteTable();
            void* expected = nullptr;
            if (!__atomic_compare_exchange_n(&metadata.compactRouteTable, &expected, static_cast<void*>(table),
                                             false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                delete table;
            }
        }
    }

    void RecordCompactRoute(size_t fromOff, MAddress dest)
    {
        EnsureCompactRouteTable();
        CompactRouteTable* table = LoadCompactRouteTable();
        CHECK(table != nullptr);
        (*table)[fromOff] = dest;
        RecordRouteStart(fromOff);
    }

    void EnsureRouteStartTable()
    {
        if (LoadRouteStartTable() == nullptr) {
            RouteStartTable* table = new RouteStartTable();
            void* expected = nullptr;
            if (!__atomic_compare_exchange_n(&metadata.routeStartTable, &expected, static_cast<void*>(table),
                                             false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                delete table;
            }
        }
    }

    void RecordRouteStart(size_t fromOff)
    {
        EnsureRouteStartTable();
        RouteStartTable* table = LoadRouteStartTable();
        CHECK(table != nullptr);
        (*table)[fromOff] = 1;
    }

    // Called only while the collector owns the route transition. Readers see
    // the table after ROUTED/FORWARDED release publication.
    void ResetRouteStartTable()
    {
        EnsureRouteStartTable();
        RouteStartTable* table = LoadRouteStartTable();
        CHECK(table != nullptr);
        table->clear();
    }

    RouteStartTable* LoadRouteStartTable() const
    {
        return static_cast<RouteStartTable*>(__atomic_load_n(&metadata.routeStartTable, __ATOMIC_ACQUIRE));
    }

    void FreeRouteStartTable()
    {
        RouteStartTable* table = static_cast<RouteStartTable*>(
            __atomic_exchange_n(&metadata.routeStartTable, static_cast<void*>(nullptr), __ATOMIC_ACQ_REL));
        delete table;
    }

    BaseObject* LookupCompactRoute(size_t fromOff, const CompactRouteTable* table) const
    {
        auto it = table->find(fromOff);
        if (it == table->end()) {
            return nullptr;
        }
        return from_region_addr(it->second);
    }

    bool IsCompactRouteDestination(MAddress address) const
    {
        // RouteState::COMPACTED is published only after CompactRegion has
        // finished recording the dense-pack table. ZGC similarly blocks page
        // access until in-place relocation is complete, then exposes the
        // relocated objects (zRelocate.cpp:862-925,1013-1037).
        if (GetRouteState() != RouteState::COMPACTED) {
            return false;
        }
        const CompactRouteTable* table = LoadCompactRouteTable();
        if (table == nullptr) {
            return false;
        }
        return std::any_of(table->begin(), table->end(),
                           [address](const auto& route) { return route.second == address; });
    }

    // A phase transition is a mutator grace period. Tables detached in generation N
    // survive two completed transitions so a detach racing the transition boundary is
    // conservatively assigned to either side without endangering a reader.
    static void AdvanceCompactRouteTableGracePeriod()
    {
        std::vector<CompactRouteTable*> ready;
        {
            std::lock_guard<std::mutex> lock(CompactRouteTableRetireMutex());
            uint64_t& generation = CompactRouteTableGraceGeneration();
            ++generation;
            auto& retired = RetiredCompactRouteTables();
            auto it = retired.begin();
            while (it != retired.end()) {
                if (generation - it->generation >= 2) {
                    ready.push_back(it->table);
                    it = retired.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (CompactRouteTable* table : ready) {
            delete table;
        }
    }

    // Deleted: asking for a route with a bare BaseObject* is unspellable.
    // Call AdmitForRoute first; product miss arms name nullopt; probes use GetRouteForProbe.
    BaseObject* GetRoute(BaseObject* fromObj) = delete;

    // Probe/diagnostics only — same Admit+derive as product, never a public bypass.
    // Precedent: GetLiveInfo0ForProbe. Anchor: ops/design/ROUTE_DOMAIN.md §2.
    BaseObject* GetRouteForProbe(BaseObject* fromObj)
    {
        OptionalRouteTicket ticket = AdmitForRoute(fromObj);
        if (!ticket) {
            return nullptr;
        }
        return GetRoute(ticket.value());
    }

    // Probe-only: pure RouteInfo geometry for a preLiveBytes rank (no survivor gate).
    MAddress GetRoutePlanAddr(uint64_t preLiveBytes)
    {
        if (!IsRouteInfoLifeCurrent()) {
            return 0;
        }
        return metadata.routeInfo.GetRoute(preLiveBytes);
    }

    ZGenerationId generation_id() const { return metadata._generation_id; }

    template<Generation G>
    void PublishFromPageMetadata(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        const RegionLifeId life = view.GetLifeId();
        CHECK_DETAIL(ForwardingTable::PublishFromPageView(
                         this, GetLiveInfo(), view.GetEpoch(), GetRegionAllocPtr(), metadata.markStartAllocPtr,
                         __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire),
                         static_cast<uint8_t>(G),
                         static_cast<uint8_t>((IsLargeRegion() ? IsCurrentFacePublished() : metadata.isMarked != 0) ||
                                              metadata.isResurrected != 0),
                         life),
                     "forwarding carrier missing at from-page publication region=%p", this);
    }

    // Product publication edge shared by forwarding and from-page liveness.
    // Keep this in the ordinary product inline path: the operation is part of
    // PrepareForwardableRegion, not a test-facing ABI surface.
    template<Generation G>
    __attribute__((always_inline)) inline void PublishForwardingCarrier(MarkView<G> view)
    {
        SetUnitRole0(static_cast<UnitRole>(metadata.unitRole));
        PublishFromPageMetadata(view);
        // zForwarding.inline.hpp:67-70 — construction token = 1. Late retain
        // after detach (count 0) is refused; carrier and token are published
        // by this single product operation.
        ZForwardingLife::ResetForForwarding(metadata.fwdRefCount, metadata.fwdClaimed, metadata.fwdDone);
        ClearForwardingFaceReset();
        ClearCurrentMarkFace();
        ZForwardingLife::reset_copy_open(metadata.copyInflight);
        metadata.routeInfo.Clear();
        metadata._generation_id = G == Generation::Young ? ZGenerationId::young : ZGenerationId::old;
        // Always install ghost membership, including a zero-live page. This is
        // what keeps the from-page carrier reachable until forwarding drain.
        SetInGhostRegion(1);
        metadata.nextRegionIdx0 = metadata.nextRegionIdx;

        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            UnitMetadata& mdata = array[i].GetMetadata();
            CHECK(static_cast<UnitRole>(mdata.unitRole) == UnitRole::SUBORDINATE_UNIT);
            CHECK(mdata.ownerRegion == this);
            CHECK(mdata.inGhostFromRegion == 0);
            array[i].SetUnitRole0(UnitRole::SUBORDINATE_UNIT);
            mdata.ownerRegion0 = this;
            array[i].SetInGhostRegion(1, GetRegionLifeId());
        }
    }

    template<Generation G>
    void PrepareForwardableRegion(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        CHECK(IsFromRegion());
        CHECK(static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS);
        CHECK(metadata.inGhostFromRegion == 0);
        // marklate: freeze last-alloc phase before ghost snapshot (survives reuse).
        AllocPhaseDiag::FreezeRegion(GetRegionStart());
        const RouteState prevRoute = GetRouteState();
        SetRouteState(FORWARDABLE);
        // After-copy Exempt keeps the page (zRelocate.cpp:1041-1047) but the
        // forwarding table must not survive into the next install.
        // EnsureEntries returns early if a table is already armed, so a kept
        // page would carry last cycle's mappings (REPORT-trainbisect §6 knife B;
        // [IKEKEEP-01] same shape). ZGC destroys forwarding at the next
        // ZRelocationSetInstallTask (zRelocationSet.cpp:91-96).
        ForwardingTable::ClearEntries(GetRegionStart(), GetRegionSize());
        CHECK_DETAIL(ForwardingTable::PreparePublicationGeneration(GetRegionStart(), GetRegionSize()),
                     "forwarding generation prepare failed region=%p range=[%#zx,%#zx)",
                     this, static_cast<size_t>(GetRegionStart()), static_cast<size_t>(GetRegionEnd()));
        // A retained page can re-enter with its route generation already
        // normalized even though copied-object headers still say FORWARDED.
        // Clear only route states known to carry such residuals; walking every
        // from is the rec=stw tax (B2.1 |Δ|=+4.53%). Snapshot prevRoute before paint.
        if (prevRoute == NORMAL || prevRoute == FORWARDED || prevRoute == COMPACTED) {
            ClearRelocationResiduals();
        }
        // PORT_ZFORWARDING step 1: same event, recorded address-keyed as well.  Populated in
        // parallel with the region machinery so the two answers can be compared before either is
        // trusted; nothing reads it for decisions yet.
        CHECK_DETAIL(ForwardingTable::InstallPublicationBeforeCopy(GetRegionStart(), GetRegionSize(), this),
                     "forwarding table install failed before relocation region=%p range=[%#zx,%#zx)",
                     this, static_cast<size_t>(GetRegionStart()), static_cast<size_t>(GetRegionEnd()));
        // enrolphase: which side of the relocate-start flip does this enrolment land on?
        //
        // OpenJDK installs the relocation set once, in the concurrent select_relocation_set
        // (zGeneration.cpp:254 ZRelocationSet::install), and only then flips
        // (relocate_start -> flip_relocate_start, :918 -> :922).  Post-flip the set is closed, so
        // "painted with the current colour after the flip" implies "will not move this cycle".
        //
        // EvacuateYoungRegions calls PrepareForwardTable<Young> twice -- WCollector.cpp:6483 and
        // again at :6952 -- with the young flip between them.  An enrolment on the far side leaves
        // a window in which a region is still NORMAL while the current colour is already the new
        // one, and a value painted there is load-good but names an object that is about to move.
        // That matches the measured FORWARD population exactly (afterFlip=1, slotGood=1, hasTo=1,
        // 20/20), and it is why moving only the first flip (kFlipAfterFromSpace) changed nothing.
        //
        // The staleness predicate is not the hole: over ~2^20 non-NORMAL targets per run, across
        // six runs, zero escaped it.
        // gc_unit fixtures do not run Heap::Init, so CollectorProxy has no
        // current collector for this diagnostic-only phase sample.  The
        // producer publication and exact-start walk remain identical; omit
        // only this non-semantic observation in the test configuration.
#if !defined(MRT_GC_UNIT_TESTS)
        NoteEnrolPhase();
#endif
        // sealcheck: snapshot is not yet sealed; geometry freeze is at RouteRegion ROUTING.
        SetMarkFaceSealed(false);
        // Shared boundary: publish immutable from-page metadata, forwarding
        // construction token, and ghost membership through one product edge.
        PublishForwardingCarrier(view);
        // Freeze exact starts while allocation headers are still readable.
        // The coverage bitmap cannot reconstruct this set after relocation.
        ResetRouteStartTable();
        (void)VisitLiveObjectsUntilFalse([this](BaseObject* object) {
            RecordRouteStart(GetAddressOffset(reinterpret_cast<MAddress>(object)));
            return true;
        });
    }

    void ClearGhostRegionBit()
    {
        if (IsGhostFromRegion()) {
            size_t nUnit = GetUnitCount();
            TraceClear::NoteRegionEvent(GetRegionStart(), nUnit * UNIT_SIZE, "clear_ghost", this,
                                        GetLiveByteCount(), 1, static_cast<unsigned int>(GetRegionType()),
                                        static_cast<unsigned int>(GetRouteState()));
            UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
            UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
            for (size_t i = 0; i < nUnit; i++) {
                array[i].SetInGhostRegion(0, GetRegionLifeId());
            }
        }
    }

    // dispel all units of this region.
    // inGhostFromRegion is the unique guard condition.

    // T-D guardian (MINOR_CONCURRENCY_0805 §八): parallel windows assert this is frozen.
    // Public for reffix parallel window assert + positive-control inject.
    static std::atomic<size_t> dispelGhostCount;
#if defined(MRT_GC_UNIT_TESTS)
    static std::atomic<GhostLookupTestHook> ghostLookupTestHook;
    static std::atomic<size_t> ghostLookupTestHookCalls;
    static void RunGhostLookupTestHook(RegionInfo* region);
    static std::atomic<RouteStateReadTestHook> routeStateReadTestHook;
    static std::atomic<size_t> routeStateReadTestHookCalls;
    static void RunRouteStateReadTestHook(RegionInfo* region);
#endif

    static size_t GetDispelGhostCount()
    {
        return dispelGhostCount.load(std::memory_order_relaxed);
    }

    // Positive control only (MRT_GCV2_REFFIX_INJECT_DISPEL=1): bump without real dispel.
    static void InjectDispelCountForTest()
    {
        dispelGhostCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DispelGhostFromRegion()
    {
        // fwdinflight: this is one of the three edges that retire from-side route state, and
        // it is unconditional -- nothing here waits for a reader. ZGC's equivalent,
        // ZForwarding::detach_page (zForwarding.cpp:171-181), blocks until _ref_count is zero.
        // Count what we would be invalidating. Default off; never blocks.

        // portmutreloc: hold the forwarding drain across the whole body. It is held
        // for the whole body so that FreeCompactRouteTable below -- ZGC's free_page -- cannot
        // run while a retained reader is inside the route lookup or a mutator copy.
        DrainScope drain(this, MutatorRelocate::Retire::DISPEL_GHOST);
        // PORT_ZFORWARDING step 1: the retirement edge.  ZGC's equivalent is refcount-driven
        // (ZForwarding::detach_page waits for _ref_count == 0); recording the removal here first
        // lets step 3 change *when* it happens without changing *where*.
        const size_t nUnit = GetGhostRegionUnitCount();
        ForwardingTable::RetireMembershipAtDispel(GetRegionStart(), GetRegionSize());
        dispelGhostCount.fetch_add(1, std::memory_order_relaxed);
        TraceClear::NoteRegionEvent(GetRegionStart(), nUnit * UNIT_SIZE, "dispel", this, GetLiveByteCount(),
                                    static_cast<unsigned int>(IsGhostFromRegion()),
                                    static_cast<unsigned int>(GetRegionType()),
                                    static_cast<unsigned int>(GetRouteState()));
        // fysfixb: name who clears the ghost bit (PrepareFromRegionList peer path).
        VLOG(REPORT,
             "[GCV2][ghost-dispel] region=%p start=%#zx nUnit=%zu live=%zu route=%u young=%u",
             this, GetRegionStart(), nUnit, GetLiveByteCount(),
             static_cast<unsigned int>(GetRouteState()),
             static_cast<unsigned>(IsYoungRegion()));
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; i++) {
            array[i].SetInGhostRegion(0, GetRegionLifeId());
        }
        // Publish route retirement before detaching the table. A reader that observes
        // the atomic nullptr then also observes NORMAL and soft-misses in GetRoute.
        SetRouteState(NORMAL);
        FreeCompactRouteTable();
        SetMarkFaceSealed(false);
        // The old top/livemap disappeared with the forwarding carrier above;
        // only page-owned ghost/route state is reset in this body.
    }

    bool IsGhostFromRegion() const
    {
        const bool ghost = metadata.regionStateBitField.GetAtomicValue(
            RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) != 0;
        if (!ghost) {
            return false;
        }
        return RegionLifeClock::Validate(RegionLifeClock::Carrier::GHOST,
                                         __atomic_load_n(&metadata.ghostLifeId, __ATOMIC_ACQUIRE),
                                         GetRegionLifeId());
    }

    // After TakeRegion re-init, every unit must have ghost cleared (payload wipe does not touch metadata).
    void AssertGhostClearedAfterReuse(size_t nUnit) const
    {
        CHECK(!IsGhostFromRegion());
        size_t baseIdx = GetUnitIdx();
        for (size_t i = 1; i < nUnit; i++) {
            MAddress addr = GetUnitAddress(baseIdx + i);
            CHECK(!InGhostFromRegion(from_region_addr(addr)));
        }
    }

    // the interface can only be used to clear live info after gc.
    // Same rule for liveInfo / liveInfo0 / retained: if the slot still holds this LiveInfo*, drop it.
    // Garbage is skipped here (may be mid-reuse); NullLiveInfoFieldsInRange covers garbage before
    // ReleaseMemory so dangling into a dying tag cannot survive.
    void CheckAndClearLiveInfo(LiveInfo* liveInfo)
    {
        // Garbage region may be reused by other thread. For the sake of safety, we don't clean it here.
        // We will clean it before the region is accessible.
        if (IsGarbageRegion()) {
            return;
        }
        // Check the value whether is expected, in order to avoid resetting a reused region.
        if (metadata.liveInfo == liveInfo) {
            SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_CHECK_AND_CLEAR, false);
            metadata.liveInfo = nullptr;
            // Tracking phase ended: live counter is no longer a mark-period truth.
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
        // Active from-page metadata is retired only by the forwarding drain.
        // Mark-arena unbinding must not invalidate an older page incarnation.
        if (metadata.retainedLiveInfo == liveInfo) {
            NoteRetainedClear(RETAINED_OP_CLEAR_CHECKED);
            metadata.retainedLiveInfo = nullptr;
            // holderlive (F2): this unbind exists because the borrowed LiveInfo* is about to
            // dangle — it says nothing about whether the snapshot is still true. When we own
            // the bits, drop the pointer and keep the verdict.
            if (metadata.retainedMarkWords != nullptr) {
                return;
            }
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
            metadata.retainedLifeId = 0;
        }
    }
    template<Generation G>
    void ClearLiveInfo(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        UnitRole unitRole = LoadUnitRole(reinterpret_cast<UnitInfo*>(this));
        if (unitRole == UnitRole::FREE_UNITS) {
            return;
        }
        CHECK_DETAIL(unitRole == UnitRole::SMALL_SIZED_UNITS || unitRole == UnitRole::LARGE_SIZED_UNITS,
                     "ClearLiveInfo must be called on a region head");
        SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_LIVE_INFO, true);
        // ZGC mark-start allocation watermark (zPage is_allocating). Capture
        // before the epoch bump so VisitLive / IsKnownEmpty / IsMarkedObject
        // see objects bumped after this point as implicitly live.
        if (MarkStartAllocWaterEnabled()) {
            metadata.markStartAllocPtr = GetRegionAllocPtr();
        } else {
            metadata.markStartAllocPtr = 0;
        }
        // Clear a large face while the supplied view still names the current
        // epoch; only then publish the epoch bump that makes old views stale.
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(view, 0);
        }
        BumpSnapshotEpochFromClearLiveInfo<G>();
        // As in ZLiveMap::set(), a new cycle owns no current face until its
        // first object is actually painted.  Clear retires the prior cycle's
        // publication; MarkObject/allocate-black publish on
        // their first 0→1 liveness write.
        ClearCurrentMarkFace();
        // A mark start publishes fresh current-page liveness. Any historical
        // from-page liveness is owned by the forwarding carrier and is not
        // detached here.
        if (metadata.liveInfo != nullptr) {
            metadata.liveInfo = nullptr;
        }
        if (G == Generation::Old) {
            NoteRetainedClear(RETAINED_OP_CLEAR_ALL);
            // A new major mark supersedes the retained major snapshot.  Young
            // clears deliberately leave this old/major authority intact.
            FreeRetainedMarkWords();
            metadata.retainedLiveInfo = nullptr;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
            metadata.retainedLifeId = 0;
            // ClearLiveInfo<Old> starts a new retained-snapshot cycle.  It is
            // the only same-region-life boundary allowed to disarm the bit.
            metadata.retainedEverPreserved = 0;
        }
        // Start of a mark cycle for this region: live=0 is authoritative until proven otherwise.
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
        SetMarkFaceSealed(false);
    }

    // Structural: drop any liveInfo/liveInfo0/retained that land in [rangeStart, rangeStart+rangeSize).
    // Called under STW immediately before ForwardDataSpace::ReleaseMemory so pointer validity
    // is a structure guarantee (not a "do not read after phase X" convention). Covers garbage.
    void NullLiveInfoFieldsInRange(uintptr_t rangeStart, size_t rangeSize)
    {
        auto inRange = [rangeStart, rangeSize](LiveInfo* p) -> bool {
            if (p == nullptr || rangeSize == 0) {
                return false;
            }
            uintptr_t addr = reinterpret_cast<uintptr_t>(p);
            return addr >= rangeStart && addr < (rangeStart + rangeSize);
        };
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(liveInfo) != LiveInfo::TEMPORARY_PTR && inRange(liveInfo)) {
            SurvNodeDiag::NoteClear(this, SurvNodeDiag::CLEAR_NULL_IN_RANGE, false);
            __atomic_store_n(&metadata.liveInfo, static_cast<LiveInfo*>(nullptr), std::memory_order_release);
            // Tracking phase for this LiveInfo ended with its backing store about to vanish.
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
        if (inRange(GetLiveInfo0ForProbe())) {
            // The livemap is inseparable from its forwarding incarnation.
            ForwardingTable::ClearEntries(GetRegionStart(), GetRegionSize());
        }
        if (inRange(metadata.retainedLiveInfo)) {
            NoteRetainedClear(RETAINED_OP_CLEAR_RANGE);
            metadata.retainedLiveInfo = nullptr;
            // holderlive (F2): same rule as CheckAndClearLiveInfo — the range is about to be
            // madvise'd, so the pointer must go; an owned copy is not in that range.
            if (metadata.retainedMarkWords != nullptr) {
                return;
            }
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
            metadata.retainedLifeId = 0;
        }
    }

    // ZForwarding::retain_page (zForwarding.cpp:86-108). Three-state: 0 refuses,
    // <0 waits for done then refuses, >0 CAS +1.
    bool RetainForwarding()
    {
        return ZForwardingLife::retain_page(metadata.fwdRefCount, metadata.fwdDone);
    }

    void ReleaseForwarding() { ZForwardingLife::release_page(metadata.fwdRefCount); }

    // ZForwarding::retain_page: the three-state count is the gate, not the list
    // type. After ForwardRegion, CollectRegion moves the region to garbage
    // while the payload is still live; mutator relocate must still pin it.
    bool TryLockReadFromRegion() { return RetainForwarding(); }

    void UnlockReadFromRegion() { ReleaseForwarding(); }

    // RAII retain_page / release_page. ok() is false when the page is already
    // released or claimed — the late reader must not touch from-side state.
    class RetainScope {
    public:
        explicit RetainScope(RegionInfo* region) : region(nullptr)
        {
            if (region != nullptr && region->RetainForwarding()) {
                this->region = region;
            }
        }
        ~RetainScope()
        {
            if (region != nullptr) {
                region->ReleaseForwarding();
            }
        }
        bool ok() const { return region != nullptr; }

        RetainScope(const RetainScope&) = delete;
        RetainScope& operator=(const RetainScope&) = delete;
        RetainScope(RetainScope&&) = delete;
        RetainScope& operator=(RetainScope&&) = delete;

    private:
        RegionInfo* region;
    };

    bool ClaimForwarding() { return ZForwardingLife::claim(metadata.fwdClaimed); }

    void MarkForwardingDone()
    {
        ZForwardingLife::mark_done(metadata.fwdDone);
    }

    bool IsForwardingDone() const { return ZForwardingLife::is_done(metadata.fwdDone); }

    bool IsForwardingFaceCurrent() const
    {
        // Prefer the current LiveInfo face, just as ZGC's page seqnum check
        // does.  A fresh face means forwarding-done is stale even if the
        // immutable from-page carrier has already retired.  Otherwise the
        // carrier epoch distinguishes a real copy (same face) from an older
        // forwarding completion; absent both identities, keep the historical
        // conservative guard.
        const uint64_t snapshotEpoch = GetSnapshotEpoch();
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo != nullptr &&
            liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) == snapshotEpoch) {
            return false;
        }
        if (HasFromPageMetadata()) {
            const ZForwarding::FromPageView* from = GetFromPageView();
            return from != nullptr && from->epoch == snapshotEpoch;
        }
        if (IsCurrentFacePublished()) {
            return false;
        }
        return true;
    }

    static constexpr uint32_t FORWARDING_FACE_RESET_BIT = (1U << 31);

    bool IsForwardingFaceReset() const
    {
        return (__atomic_load_n(&metadata.retainedPreserveCnt, __ATOMIC_ACQUIRE) &
            FORWARDING_FACE_RESET_BIT) != 0;
    }

    void SetForwardingFaceReset()
    {
        (void)__atomic_fetch_or(&metadata.retainedPreserveCnt, FORWARDING_FACE_RESET_BIT, __ATOMIC_ACQ_REL);
    }

    void ClearForwardingFaceReset()
    {
        (void)__atomic_fetch_and(&metadata.retainedPreserveCnt, ~FORWARDING_FACE_RESET_BIT, __ATOMIC_ACQ_REL);
    }

    void PublishCurrentMarkFace()
    {
        CHECK(GetSnapshotEpoch() != 0);
        (void)__atomic_fetch_or(&metadata.snapshotEpoch, 1ULL, __ATOMIC_RELEASE);
    }

    void ClearCurrentMarkFace()
    {
        if (IsLargeRegion()) {
            (void)__atomic_fetch_and(&metadata.liveByteCount, ~LIVE_FACE_PUBLISHED_BIT, __ATOMIC_ACQ_REL);
        }
        (void)__atomic_fetch_and(&metadata.snapshotEpoch, ~1ULL, __ATOMIC_ACQ_REL);
    }

    bool IsCurrentFacePublished() const
    {
        if (IsLargeRegion()) {
            return (__atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire) &
                    LIVE_FACE_PUBLISHED_BIT) != 0;
        }
        return (__atomic_load_n(&metadata.snapshotEpoch, __ATOMIC_ACQUIRE) & 1ULL) != 0;
    }

    // Large pages have one liveness bit, but Preserve also consumes their byte
    // count. Keep publication and count in the same atomic word so a reader
    // cannot capture SNAPSHOT_VALID between those writes. The old publication
    // bit identifies the sole first-paint winner.
    template<Generation G>
    bool TryPublishLargeFace(MarkView<G> view, uint64_t liveBytes)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        CHECK(liveBytes <= LIVE_BYTES_MASK);
        uint64_t observed = __atomic_load_n(&metadata.liveByteCount, __ATOMIC_ACQUIRE);
        for (;;) {
            if ((observed & LIVE_FACE_PUBLISHED_BIT) != 0) {
                return false;
            }
            const uint64_t bytes = observed & LIVE_BYTES_MASK;
            CHECK(liveBytes <= LIVE_BYTES_MASK - bytes);
            const uint64_t next = observed | LIVE_FACE_PUBLISHED_BIT | liveBytes |
                (liveBytes == 0 ? 0 : LIVE_AUTHORITY_BIT);
            if (__atomic_compare_exchange_n(&metadata.liveByteCount, &observed, next, true,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return true;
            }
        }
    }

    // ZGC has no terminal kept: a page not selected this cycle is an ordinary
    // candidate next cycle (zRelocationSetSelector.cpp:114-196 rebuilds from
    // the page table; zGeneration.cpp:205-213). Drop the in-cycle publish so
    // WaitRoutedTipReady cannot treat last cycle's Exempt as this cycle's done.
    void ExpireKeptPublish()
    {
        if (IsGhostFromRegion()) {
            DispelGhostFromRegion();
        } else {
            const RouteState rs = GetRouteState();
            if (rs != RouteState::FORWARDED && rs != RouteState::COMPACTED && rs != RouteState::NORMAL) {
                SetRouteState(NORMAL);
            }
            // The non-ghost expiry arm is still a forwarding-life boundary.
            // Seal before resetting the carrier words so an admitted copier
            // cannot be relabelled as belonging to the next life.
            WaitCopiedInflight();
        }
        ZForwardingLife::ResetIdle(metadata.fwdRefCount, metadata.fwdClaimed, metadata.fwdDone);
        ClearForwardingFaceReset();
        ClearCurrentMarkFace();
        ZForwardingLife::reset_copy_sealed(metadata.copyInflight);
    }

    bool BeginCopyAdmission() { return ZForwardingLife::begin_copy(metadata.copyInflight); }

    void CommitCopyAdmission() { ZForwardingLife::commit_copy(metadata.copyInflight); }

    bool NoteCopyInflight() { return ZForwardingLife::note_copy(metadata.copyInflight); }

    void EndCopyInflight() { ZForwardingLife::end_copy(metadata.copyInflight); }

    void WaitCopiedInflight() { ZForwardingLife::wait_copied(metadata.copyInflight); }

    int32_t CopyInflight() const { return ZForwardingLife::copy_count(metadata.copyInflight); }

    ZForwardingLife::CopyAdmissionState CopyAdmission() const
    {
        return ZForwardingLife::copy_admission_state(metadata.copyInflight);
    }

    int32_t ForwardingRefCount() const { return metadata.fwdRefCount.load(std::memory_order_acquire); }

    bool ForwardingClaimed() const { return metadata.fwdClaimed.load(std::memory_order_acquire); }

    void LockWriteRegion() { metadata.rwLock.LockWrite(); }

    void UnlockWriteRegion() { metadata.rwLock.UnlockWrite(); }

    // ZForwarding in_place_relocation_claim_page + detach_page (zForwarding.cpp:110-181).
    // Invert the count (n → -n) so new retainers refuse, wait until -1 (every reader
    // has released), then the retire body runs exclusive. Destructor publishes done
    // and drops the construction token to 0. Always on.
    class DrainScope {
    public:
        // Keep the retire-side admission/drain linearization in the product
        // library. A header definition lets every consumer, including a test
        // executable, instantiate a private weak copy that a rebuilt runtime
        // cannot control or verify.
        MRT_EXPORT DrainScope(RegionInfo* region, MutatorRelocate::Retire site);

        ~DrainScope()
        {
            if (region == nullptr) {
                return;
            }
            region->MarkForwardingDone();
            if (region->metadata.fwdRefCount.load(std::memory_order_acquire) != 0) {
                ZForwardingLife::release_page(region->metadata.fwdRefCount);
            }
        }

        DrainScope(const DrainScope&) = delete;
        DrainScope& operator=(const DrainScope&) = delete;
        DrainScope(DrainScope&&) = delete;
        DrainScope& operator=(DrainScope&&) = delete;

    private:
        RegionInfo* region;
    };

    // These interfaces are used to make sure the writing operations of value in C++ Bit Field will be atomic.
    void SetUnitRole(UnitRole role)
    {
        metadata.unitRoleBitField.SetAtomicValue(0, BIT_LENGTH, static_cast<uint8_t>(role));
    }
    void SetUnitRole0(UnitRole role)
    {
        metadata.unitRoleBitField.SetAtomicValue(BIT_LENGTH, BIT_LENGTH, static_cast<uint8_t>(role));
    }
    void SetRegionType(RegionType type)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH,
                                                    static_cast<uint8_t>(type));
        // PORT_ZFORWARDING step 1, continued.  Inserting only at PrepareForwardableRegion left the
        // table 15.6% short of what the region predicates answer (legacyOnly=2.6M of 16.8M), because
        // membership of the relocation set is established at many scattered sites here --
        // fromRegionList.PrependRegion(..., FROM_REGION) appears in AssembleSmallGarbageCandidates,
        // PrepareYoungGarbageCandidates and several others -- while ZGC establishes it once, in
        // ZRelocationSet::install.
        //
        // Rather than chase those sites one by one, hook the single place the type actually
        // changes.  That is the convergence the port is for: one writer of membership instead of N.
        if (type == RegionType::FROM_REGION || type == RegionType::LONE_FROM_REGION ||
            type == RegionType::UNMOVABLE_FROM_REGION || type == RegionType::RAW_POINTER_PINNED_REGION) {
            // Best-effort comparison carrier. Correctness is established by
            // the checked full install in PrepareForwardableRegion; tests and
            // pre-heap metadata transitions may legitimately have no map yet.
            (void)ForwardingTable::InsertProvisional(GetRegionStart(), GetRegionSize(), this);
        } else if (type == RegionType::FREE_REGION || type == RegionType::GARBAGE_REGION ||
                   type == RegionType::TO_REGION) {
            // The ghost bit is part of membership and outlives the type change: all six residual
            // legacyOnly disagreements were rtype=14 (GARBAGE_REGION) with ghost still set, i.e.
            // the type moved on while IsGhostFromObject still answered yes.  Dropping the entry
            // there is exactly the "membership has no single source of truth" problem this port
            // exists to remove, so keep it until the ghost is actually dispelled
            // (DispelGhostFromRegion already calls Remove).
            if (!IsGhostFromRegion()) {
                ForwardingTable::Remove(GetRegionStart(), GetRegionSize());
            }
        }
    }
    void SetTraceRegionFlag(uint8_t flag)
    {
        uint8_t prev = metadata.isTraceRegion;
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::TRACE_REGION_FLAG, 1, flag);
        // blackmark: track 1→0 clears (EnlistFullThreadLocalRegion / HandleTraceRegions).
        if (AllocPhaseDiag::Enabled()) {
            if (flag == 0 && prev != 0) {
                AllocPhaseDiag::NoteTraceFlagCleared(GetRegionStart());
            } else if (flag != 0) {
                AllocPhaseDiag::NoteTraceFlagSet(GetRegionStart());
            }
        }
    }
    // twoflags: CSet/route exclusion only. Independent of isTraceRegion lifetime.
    void SetNotRelocatableThisCycle(uint8_t flag)
    {
        __atomic_store_n(&metadata.notRelocatableThisCycle, flag, __ATOMIC_RELEASE);
    }
    bool IsNotRelocatableThisCycle() const
    {
        return __atomic_load_n(&metadata.notRelocatableThisCycle, __ATOMIC_ACQUIRE) != 0;
    }
    // routedest: destination-side hold. Idempotent by construction, and it has to be:
    // RouteOrCompactRegionImpl publishes the split plan twice for the same holder when the
    // toRegion2 allocation fails (SetRouteInfo at RegionManager.cpp:1992, then again at
    // :1999), so the stamp on toRegion1 runs twice. Stamping a byte twice is a no-op.
    // Do NOT turn this into a reference count without handling that double publish.
    void SetRouteDestHold(uint8_t flag)
    {
        uint8_t cur = __atomic_load_n(&metadata.routeDestHold, __ATOMIC_RELAXED);
        uint8_t next = static_cast<uint8_t>((cur & ~1u) | (flag != 0 ? 1u : 0u));
        __atomic_store_n(&metadata.routeDestHold, next, __ATOMIC_RELEASE);
    }
    bool IsRouteDestHeld() const
    {
        return (__atomic_load_n(&metadata.routeDestHold, __ATOMIC_ACQUIRE) & 1u) != 0;
    }
    void SetInGhostRegion(uint8_t flag)
    {
        const RegionLifeId life = GetRegionLifeId();
        __atomic_store_n(&metadata.ghostLifeId, life, __ATOMIC_RELEASE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
        if (flag != 0) {
            RegionLifeClock::Publish(RegionLifeClock::Carrier::GHOST, life);
        }
    }

    void SetOldMarkedRegionFlag(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1, flag);
    }

    void SetEnqueuedRegionFlag(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::ENQUEUED_REGION_FLAG, 1, flag);
    }
    void SetResurrectedRegionFlag(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::RESURRECTED_REGION_FLAG, 1, flag);
    }

    void SetYoungRegionFlag(uint8_t flag);

    bool IsYoungRegion() const
    {
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::YOUNG_REGION_FLAG, 1) != 0;
    }

    static size_t GetYoungRegionCount();

    static bool HasYoungRegions();

    // Promotion replaces current page metadata instead of retargeting the same
    // liveness object. The old Young metadata remains available only through the
    // from-page carrier; the new Old current metadata starts with no livemap.
    MarkView<Generation::Old> PromoteYoungRegion(MarkView<Generation::Young> youngView)
    {
        CHECK_DETAIL(youngView.GetRegion() == this, "young promotion view belongs to another region");
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        CHECK_DETAIL(youngView.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>(),
                     "cannot promote region %p through a stale young mark view", this);
        __atomic_store_n(&metadata.liveInfo, static_cast<LiveInfo*>(nullptr), std::memory_order_release);
        SetOldMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
        metadata.markStartAllocPtr = 0;
        BumpSnapshotEpoch();
        SetYoungRegionFlag(0);
        SetYoungAge(0);
        return GetMarkView<Generation::Old>();
    }

    void SetYoungAge(uint8_t age)
    {
        CHECK(age <= MAX_YOUNG_AGE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH, age);
    }

    uint8_t GetYoungAge() const
    {
        return static_cast<uint8_t>(metadata.regionStateBitField.GetAtomicValue(
                                        RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BIT_LENGTH) >>
                                    RegionStateBitPos::YOUNG_AGE_FLAG);
    }

    RegionType GetRegionType() const
    {
        return static_cast<RegionType>(
            metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH));
    }
    UnitRole GetUnitRole() const { return static_cast<UnitRole>(metadata.unitRole); }

    size_t GetUnitIdx() const { return RegionInfo::UnitInfo::GetUnitIdx(reinterpret_cast<const UnitInfo*>(this)); }

    MAddress GetRegionStart() const
    {
        uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
        CHECK(ptr < UnitInfo::heapStartAddress);
        size_t idx = (UnitInfo::heapStartAddress - ptr) / sizeof(UnitInfo) - 1;
        CHECK(idx < UnitInfo::totalUnitCount);
        return idx * UNIT_SIZE + UnitInfo::heapStartAddress;
    }

    MAddress GetRegionEnd() const { return metadata.regionEnd; }

    void SetRegionAllocPtr(MAddress addr) { metadata.allocPtr = addr; }

    MAddress GetRegionAllocPtr() const { return metadata.allocPtr; }

    MAddress GetMarkStartAllocPtr() const { return metadata.markStartAllocPtr; }

    // Product on. MRT_GCV2_MARKWATER_OFF=1 restores the pre-watermark trickle
    // (perturbation for REPORT-oracleblack3 §4).
    static bool MarkStartAllocWaterEnabled()
    {
        static const bool on = []() {
            const char* v = std::getenv("MRT_GCV2_MARKWATER_OFF");
            return v == nullptr || !(v[0] == '1' && v[1] == '\0');
        }();
        return on;
    }

    // offset ≥ mark-start allocPtr (exclusive end at ClearLiveInfo). Objects
    // bumped after that point are ZGC allocate-black / is_allocating.
    // water == start means the region was empty at mark-start, so every
    // object now in it was born after that snapshot.
    bool AllocatedAfterMarkStart(size_t offset) const
    {
        if (!MarkStartAllocWaterEnabled()) {
            return false;
        }
        uintptr_t water = metadata.markStartAllocPtr;
        if (water == 0) {
            return false;
        }
        MAddress start = GetRegionStart();
        if (water <= start) {
            return true;
        }
        return offset >= static_cast<size_t>(water - start);
    }

    bool HasMarkStartAllocGap() const
    {
        if (!MarkStartAllocWaterEnabled()) {
            return false;
        }
        uintptr_t water = metadata.markStartAllocPtr;
        if (water == 0) {
            return false;
        }
        return GetRegionAllocPtr() > water;
    }

    int32_t IncRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_add(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount >= 0, "region %p has wrong raw pointer count %d", this);
        CHECK_DETAIL(oldCount < MAX_RAW_POINTER_COUNT, "inc raw-pointer-count overflow");
        return oldCount;
    }

    int32_t DecRawPointerObjectCount()
    {
        int32_t oldCount = __atomic_fetch_sub(&metadata.rawPointerObjectCount, 1, __ATOMIC_SEQ_CST);
        CHECK_DETAIL(oldCount > 0, "dec raw-pointer-count underflow, please check whether releaseRawData is overused.");
        return oldCount;
    }

    int32_t GetRawPointerObjectCount() const
    {
        return __atomic_load_n(&metadata.rawPointerObjectCount, __ATOMIC_SEQ_CST);
    }

    bool CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal)
    {
        return __atomic_compare_exchange_n(&metadata.rawPointerObjectCount, &expectVal, newVal, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_ACQUIRE);
    }

    uintptr_t Alloc(size_t size)
    {
        size_t limit = GetRegionEnd();
        if (metadata.allocPtr + size <= limit) {
            uintptr_t addr = metadata.allocPtr;
            metadata.allocPtr += size;
            return addr;
        } else {
            return 0;
        }
    }

    // for regions shared by multithreads
    uintptr_t AtomicAlloc(size_t size)
    {
        uintptr_t addr = __atomic_fetch_add(&metadata.allocPtr, size, __ATOMIC_ACQ_REL);
        // should not check allocPtr, because it might be shared
        if ((addr < GetRegionEnd()) && (size <= GetRegionEnd() - addr)) {
            return addr;
        }
        if (addr <= GetRegionEnd()) {
            __atomic_store_n(&metadata.allocPtr, addr, __ATOMIC_SEQ_CST);
        }
        return 0;
    }

    bool IsTraceRegion() const { return metadata.isTraceRegion == 1; }

    // copyable during concurrent copying gc.
    bool IsSmallRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS; }

    bool IsLargeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS; }

    bool IsThreadLocalRegion() const
    {
        return static_cast<RegionType>(metadata.regionType) == RegionType::THREAD_LOCAL_REGION;
    }

    bool IsPinnedRegion() const
    {
        return (static_cast<RegionType>(metadata.regionType) == RegionType::FULL_PINNED_REGION) ||
            (static_cast<RegionType>(metadata.regionType) == RegionType::RECENT_PINNED_REGION);
    }

    RegionInfo* GetPrevRegion() const
    {
        if (UNLIKELY(metadata.prevRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.prevRegionIdx));
    }

    // Intrusive-list authority. A region has at most one owning RegionList;
    // ghost snapshots intentionally do not modify this token.
    RegionList* GetRegionListOwner() const { return metadata.regionListOwner.load(std::memory_order_acquire); }

    void SetRegionListOwner(RegionList* owner) { metadata.regionListOwner.store(owner, std::memory_order_release); }

    void SetPrevRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.prevRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t prevIdx = r->GetUnitIdx();
        MRT_ASSERT(prevIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.prevRegionIdx = static_cast<uint32_t>(prevIdx);
    }

    RegionInfo* GetNextRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx < UnitInfo::totalUnitCount);
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.nextRegionIdx));
    }

    RegionInfo* GetNextGhostRegion() const
    {
        if (UNLIKELY(metadata.nextRegionIdx0 == NULLPTR_IDX)) {
            return nullptr;
        }
        DCHECK(metadata.nextRegionIdx0 < UnitInfo::totalUnitCount);
        return reinterpret_cast<RegionInfo*>(UnitInfo::GetUnitInfo(metadata.nextRegionIdx0));
    }

    void SetNextRegion(const RegionInfo* r)
    {
        if (UNLIKELY(r == nullptr)) {
            metadata.nextRegionIdx = NULLPTR_IDX;
            return;
        }
        size_t nextIdx = r->GetUnitIdx();
        MRT_ASSERT(nextIdx < NULLPTR_IDX, "exceeds the maximum limit for region info");
        metadata.nextRegionIdx = static_cast<uint32_t>(nextIdx);
    }

    bool IsFromRegion() const { return GetRegionType() == RegionType::FROM_REGION; }
    bool IsLoneFromRegion() const { return GetRegionType() == RegionType::LONE_FROM_REGION; }
    bool IsUnmovableFromRegion() const
    {
        RegionType type = GetRegionType();
        return type == RegionType::UNMOVABLE_FROM_REGION || type == RegionType::RAW_POINTER_PINNED_REGION;
    }

    bool IsToRegion() const { return GetRegionType() == RegionType::TO_REGION; }

    bool IsGarbageRegion() const { return GetRegionType() == RegionType::GARBAGE_REGION; }
    bool IsFreeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::FREE_UNITS; }

    bool IsValidRegion() const
    {
        return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS ||
            static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS;
    }

    // liveByteCount: bit63 = LIVE_AUTHORITY, bit62 = large-face publication,
    // bits0-61 = live bytes.  The large path updates the latter two fields in
    // one atomic word; small pages continue to use the seqnum publication bit.
    // densify / fragmentation still use the byte count; reclaim-empty uses IsKnownEmpty()
    // which mirrors ZGC page->is_marked() (mark face epoch), not the byte counter alone.
    static constexpr uint64_t LIVE_AUTHORITY_BIT = 1ull << 63;
    static constexpr uint64_t LIVE_FACE_PUBLISHED_BIT = 1ull << 62;
    static constexpr uint64_t LIVE_BYTES_MASK = LIVE_FACE_PUBLISHED_BIT - 1ull;

    // livesame crosscheck (ZGC ZPage::verify_live): live book vs mark face.
    static std::atomic<size_t> liveCrossMismatchCount;
    static std::atomic<size_t> liveCrossCheckCount;
    static std::atomic<bool> liveCrossAtexitInstalled;

    uint64_t GetLiveByteCount() const
    {
        return __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire) & LIVE_BYTES_MASK;
    }

    bool IsLiveCountAuthoritative() const
    {
        return (__atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire) & LIVE_AUTHORITY_BIT) != 0;
    }

    // ZGC zGeneration.cpp:216-221 / zPage.inline.hpp:223-225:
    //   is_marked = livemap.seqnum == generation.seqnum
    //   register_empty_page iff !is_marked — but that is safe only because ZGC's mark
    //   is complete for every relocatable page. Ours is not (GetRouteMarkView mints
    //   epoch from liveInfo0; stale_read viewEpoch≠snapshotEpoch).
    // cjpmnull2: empty = this-cycle marked ∧ live==0. Epoch mismatch / null face
    // means "not marked this cycle", not "empty". Authority still required so a
    // minor cannot reclaim non-young on a bare zero.
    bool IsKnownEmpty(MarkView<Generation::Old> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        // ZGC allocate-black: a mark-start watermark gap means objects were
        // born after ClearLiveInfo and are implicitly live. Do not treat the
        // region empty, and do not AddLiveByteCount for them.
        if (HasMarkStartAllocGap()) {
            return false;
        }
        uint64_t raw = __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
        const bool auth = (raw & LIVE_AUTHORITY_BIT) != 0;
        bool markedThisCycle = false;
        bool keepNullFace = false;
        bool keepEpoch = false;
        if (IsLargeRegion()) {
            if (view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Old>()) {
                keepEpoch = true;
            } else {
                markedThisCycle = true;
            }
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr || reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
                markedThisCycle = false;
                keepNullFace = true;
            } else if (liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) !=
                           view.GetEpoch() ||
                       view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Old>()) {
                markedThisCycle = false;
                keepEpoch = true;
            } else {
                markedThisCycle = true;
            }
        }
        const bool emptyByMark = markedThisCycle && (IsLargeRegion()
            ? GetMarkedRegionFlag(view) == 0
            : ((raw & LIVE_BYTES_MASK) == 0));
        if (OneseqDiagEnabled()) {
            oneseqIsKnownEmptyCalls.fetch_add(1, std::memory_order_relaxed);
            if (!auth && emptyByMark) {
                oneseqAuthBlocksReclaim.fetch_add(1, std::memory_order_relaxed);
            } else if (auth && emptyByMark) {
                oneseqAuthAndEmpty.fetch_add(1, std::memory_order_relaxed);
            } else if (auth && !emptyByMark) {
                oneseqAuthNotEmpty.fetch_add(1, std::memory_order_relaxed);
            } else {
                oneseqNoAuthNotEmpty.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!ikeAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr,
                             "[GCV2][ike-keep] atexit trueEmpty=%zu keep=%zu keepBytes=%zu "
                             "nullFace=%zu epoch=%zu\n",
                             ikeTrueEmpty.load(std::memory_order_relaxed),
                             ikeConservativeKeep.load(std::memory_order_relaxed),
                             ikeConservativeKeepBytes.load(std::memory_order_relaxed),
                             ikeNullFaceKeep.load(std::memory_order_relaxed),
                             ikeEpochKeep.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
        }
        if (!auth) {
            return false;
        }
        if (emptyByMark) {
            ikeTrueEmpty.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (keepNullFace || keepEpoch) {
            size_t n = ikeConservativeKeep.fetch_add(1, std::memory_order_relaxed) + 1;
            ikeConservativeKeepBytes.fetch_add(GetRegionSize(), std::memory_order_relaxed);
            if (keepNullFace) {
                ikeNullFaceKeep.fetch_add(1, std::memory_order_relaxed);
            }
            if (keepEpoch) {
                ikeEpochKeep.fetch_add(1, std::memory_order_relaxed);
            }
            if (n <= 8 || (n & (n - 1)) == 0) {
                LOG(RTLOG_ERROR,
                    "[GCV2][ike-keep] n=%zu region=%p start=%#zx nullFace=%u epoch=%u "
                    "live=%llu — not empty (unmarked this cycle)",
                    n, this, GetRegionStart(), static_cast<unsigned>(keepNullFace),
                    static_cast<unsigned>(keepEpoch),
                    static_cast<unsigned long long>(raw & LIVE_BYTES_MASK));
            }
        }
        return false;
    }

    bool IsKnownYoungEmpty(MarkView<Generation::Young> view) const
    {
        CHECK(view.GetRegion() == this);
        if (!ValidateMarkView(view)) {
            return false;
        }
        if (HasMarkStartAllocGap()) {
            return false;
        }
        uint64_t raw = __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
        const bool auth = (raw & LIVE_AUTHORITY_BIT) != 0;
        bool markedThisCycle = false;
        if (IsLargeRegion()) {
            markedThisCycle = view.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>();
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr || reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
                markedThisCycle = false;
            } else {
                markedThisCycle = liveInfo->GetMarkFace().epoch.load(std::memory_order_acquire) ==
                        view.GetEpoch() &&
                    view.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>();
            }
        }
        const bool emptyByMark = markedThisCycle && (IsLargeRegion()
            ? GetMarkedRegionFlag(view) == 0
            : ((raw & LIVE_BYTES_MASK) == 0));
        return auth && emptyByMark;
    }

    bool IsSafeKnownEmpty(MarkView<Generation::Old> view)
    {
        if (!IsKnownEmpty(view)) {
            return false;
        }
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            return true;
        }
        // Examined: either large, or we had a mark face this cycle that is now stale/null
        // (authority already required by IsKnownEmpty). Residual bitmap pointer may remain.
        return GetMarkBitmap(view) != nullptr || GetResurrectBitmap() != nullptr || IsLargeRegion() ||
            __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire) != nullptr;
    }

    bool IsSafeKnownYoungEmpty(MarkView<Generation::Young> view)
    {
        if (!IsKnownYoungEmpty(view)) {
            return false;
        }
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            return true;
        }
        return GetMarkBitmap(view) != nullptr || IsLargeRegion() ||
            __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire) != nullptr;
    }

    void ResetLiveByteCount()
    {
        // densify rebuild: clear byte counter only (mark face rewritten in place next).
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
    }

    // ZGC zForwarding.cpp:71-74 reset_livemap after from-page iteration — one publish:
    // empty live bytes + invalidate mark face (bump snapshotEpoch; large clears isMarked).
    // MARK_EPOCH_DISCIPLINE §4.2: no memset of shared markWords.
    template<Generation G>
    void ResetLiveMapAfterForward(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
        // Forwarding is the last reader of this mark face. Copy it while the
        // supplied view is still current; a partially forwarded page may stay
        // UNMOVABLE_FROM after the epoch bump and still contain live holders.
        if (!IsLargeRegion()) {
            PreserveRetainedLiveInfo();
        }
        SetForwardingFaceReset();
        ClearCurrentMarkFace();
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(view, 0);
        }
        BumpSnapshotEpochFromResetAfterForward<G>();
    }

    void AddLiveByteCount(uint64_t count)
    {
        CHECK(count <= LIVE_BYTES_MASK);
        uint64_t observed = __atomic_load_n(&metadata.liveByteCount, __ATOMIC_ACQUIRE);
        for (;;) {
            const uint64_t bytes = observed & LIVE_BYTES_MASK;
            CHECK(count <= LIVE_BYTES_MASK - bytes);
            const uint64_t next = (observed & ~LIVE_BYTES_MASK) | (bytes + count) |
                LIVE_AUTHORITY_BIT;
            if (__atomic_compare_exchange_n(&metadata.liveByteCount, &observed, next, true,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return;
            }
        }
    }

    // ZGC ZPage::verify_live — live_objects/bytes must match livemap. Always-on counter;
    // MRT_GCV2_LIVE_CROSSCHECK=1 aborts on mismatch.
    template<Generation G>
    void VerifyLiveBooks(MarkView<G> view, const char* where)
    {
        CHECK(view.GetRegion() == this);
        liveCrossCheckCount.fetch_add(1, std::memory_order_relaxed);
        if (!IsLiveCountAuthoritative()) {
            return;
        }
        const uint64_t liveBytes = GetLiveByteCount();
        const bool emptyByMark = G == Generation::Young
            ? IsKnownYoungEmpty(MarkView<Generation::Young>(this, view.GetEpoch(), view.GetLifeId()))
            : IsKnownEmpty(MarkView<Generation::Old>(this, view.GetEpoch(), view.GetLifeId()));
        // Homology only when this cycle marked the page. Unmarked ∧ live==0 is
        // conservative-keep (cjpmnull2), not a book error (zPage.inline.hpp:223-225).
        const bool emptyByLive = (liveBytes == 0);
        if (emptyByMark == emptyByLive || (!emptyByMark && emptyByLive)) {
            return;
        }
        size_t n = liveCrossMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!liveCrossAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr, "[GCV2][livesame][crosscheck] atexit checks=%zu mismatch=%zu\n",
                             liveCrossCheckCount.load(std::memory_order_relaxed),
                             liveCrossMismatchCount.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
        }
        if (n <= 32) {
            LOG(RTLOG_ERROR,
                "[GCV2][livesame][crosscheck] where=%s region=%p liveBytes=%llu emptyByMark=%u "
                "emptyByLive=%u n=%zu",
                where != nullptr ? where : "?", this, static_cast<unsigned long long>(liveBytes),
                static_cast<unsigned>(emptyByMark), static_cast<unsigned>(emptyByLive), n);
        }
    }

    void RemoveFromList()
    {
        RegionInfo* prev = GetPrevRegion();
        RegionInfo* next = GetNextRegion();
        if (prev != nullptr) {
            prev->SetNextRegion(next);
        }
        if (next != nullptr) {
            next->SetPrevRegion(prev);
        }
        this->SetNextRegion(nullptr);
        this->SetPrevRegion(nullptr);
    }

private:
    struct RetiredCompactRouteTable {
        CompactRouteTable* table;
        uint64_t generation;
    };

    CompactRouteTable* LoadCompactRouteTable() const
    {
        return static_cast<CompactRouteTable*>(
            __atomic_load_n(&metadata.compactRouteTable, __ATOMIC_ACQUIRE));
    }

    static std::mutex& CompactRouteTableRetireMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static uint64_t& CompactRouteTableGraceGeneration()
    {
        static uint64_t generation = 0;
        return generation;
    }

    static std::vector<RetiredCompactRouteTable>& RetiredCompactRouteTables()
    {
        static std::vector<RetiredCompactRouteTable> retired;
        return retired;
    }

    static void RetireCompactRouteTable(CompactRouteTable* table)
    {
        std::lock_guard<std::mutex> lock(CompactRouteTableRetireMutex());
        RetiredCompactRouteTables().push_back({ table, CompactRouteTableGraceGeneration() });
    }

    // Product geometry only — reachable from GetRoute(RouteTicket). External product
    // callers cannot reach preLiveBytes without a ticket (ROUTE_DOMAIN.md §2).
    size_t GetPreLiveBytesInGhostRegion(MAddress address)
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        DCHECK(from != nullptr && from->liveInfo != nullptr);
        size_t offset = GetAddressOffset(address);
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            return from->liveInfo->GetPreLiveBytes(view, offset, GetGhostRegionSize());
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        return from->liveInfo->GetPreLiveBytes(view, offset, GetGhostRegionSize());
    }

    ALWAYS_INLINE void CheckObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const
    {
        // Always-on TypeInfo range check: same predicate as CheckTypeInfoRegion rule 3
        // (VerifyHeap.cpp:105-108) — tip ∈ heap address range is a defect.
        // Default: count + one-shot dump (no abort). Fatal: MRT_GCV2_TIPINHEAP_FATAL=1.
        TypeInfo* tip = obj->GetTypeInfo();
        if (UNLIKELY(Heap::IsHeapAddress(tip))) {
            ReportTypeInfoInHeap(obj, tip, objSize, regionStart, regionEnd);
        }
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        // kMarkedBytesPerBit is 8, matching Allocator::ALLOC_ALIGN (Allocator.h:19).
        if (UNLIKELY(objSize == 0 || (objSize % kMarkedBytesPerBit) != 0 || objSize > regionEnd - objAddr)) {
            ReportInvalidObjectSize(obj, objSize, regionStart, regionEnd);
        }
    }

    // Cold path for tip ∈ heap. Reuses Heap::IsHeapAddress (CheckTypeInfoRegion rule 3 body);
    // does not reimplement the full VERIFY_HEAP channel (stats / misaligned / ContainsAddress).
    ATTR_COLD ATTR_NO_INLINE void ReportTypeInfoInHeap(const BaseObject* obj, TypeInfo* tip, size_t objSize,
                                                       MAddress regionStart, MAddress regionEnd) const
    {
        size_t n = tipInHeapHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            LOG(RTLOG_ERROR,
                "[GCV2][tipguard][TYPEINFO_IN_HEAP] obj=%p tip=%p objSize=%zu region=%p regionStart=%#zx "
                "regionEnd=%#zx allocPtr=%#zx regionType=%u young=%u phase=%u "
                "(default=count; fatal=MRT_GCV2_TIPINHEAP_FATAL=1)",
                obj, tip, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(),
                static_cast<unsigned>(GetRegionType()), static_cast<unsigned>(IsYoungRegion()),
                static_cast<unsigned>(phase));
        } else if ((n & 0x3ffU) == 0) {
            LOG(RTLOG_ERROR, "[GCV2][tipguard][TYPEINFO_IN_HEAP_COUNT] total=%zu", n);
        }
    }

    NO_RETURN ATTR_COLD ATTR_NO_INLINE void ReportInvalidObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const
    {
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        size_t bitCapacity = (regionEnd - regionStart) / kMarkedBytesPerBit;
        size_t bitIndex = objAddr >= regionStart ? (objAddr - regionStart) / kMarkedBytesPerBit :
                                                   std::numeric_limits<size_t>::max();
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        LOG(RTLOG_FATAL,
            "[GCV2][sizeguard][INVALID_OBJECT_SIZE] obj=%p objSize=%zu region=%p regionStart=%#zx "
            "regionEnd=%#zx allocPtr=%#zx regionType=%u unitRole=%u young=%u phase=%u bitCap=%zu bitIdx=%zu align=%zu",
            obj, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(), static_cast<unsigned>(GetRegionType()),
            static_cast<unsigned>(GetUnitRole()), static_cast<unsigned>(IsYoungRegion()),
            static_cast<unsigned>(phase), bitCapacity, bitIndex, kMarkedBytesPerBit);
        std::abort();
    }

    static std::atomic<size_t> tipInHeapHits;

    static std::atomic<size_t> youngRegionCount;
    static std::mutex youngRegionFlagMutex;
    static constexpr int32_t MAX_RAW_POINTER_COUNT = std::numeric_limits<int32_t>::max();
    static constexpr int32_t BIT_LENGTH = 4;
    static constexpr uint8_t YOUNG_AGE_BIT_LENGTH = 6;
    static constexpr uint8_t YOUNG_STATE_BIT_LENGTH = 1 + YOUNG_AGE_BIT_LENGTH;
    static constexpr uint8_t MAX_YOUNG_AGE = (1U << YOUNG_AGE_BIT_LENGTH) - 1;
    static constexpr uint8_t MARK_FACE_SEALED_BIT = 1U << 0;
    enum RegionStateBitPos : uint8_t {
        REGION_TYPE_FLAG = 0,
        TRACE_REGION_FLAG = BIT_LENGTH,
        IN_GHOST_FROM_REGION_FLAG,
        MARKED_REGION_FLAG,
        ENQUEUED_REGION_FLAG,
        RESURRECTED_REGION_FLAG,
        YOUNG_REGION_FLAG,
        YOUNG_AGE_FLAG
    };

    struct UnitMetadata {
        struct { // basic data for RegionInfo
            // for fast allocation, always at the start.
            uintptr_t allocPtr;
            uintptr_t regionEnd;

            uint32_t nextRegionIdx;
            uint32_t prevRegionIdx; // support fast deletion for region list.

            uint64_t liveByteCount;
            int32_t rawPointerObjectCount;
            uint32_t censusBoundaryOffset;
        };

        // Authoritative intrusive-list membership; ghost snapshots do not claim it.
        std::atomic<RegionList*> regionListOwner{ nullptr };

        // ZGC page seqnum analogue: an independent, non-wrapping incarnation
        // identity. It is deliberately not packed into routeDestHold.
        std::atomic<RegionLifeId> regionLifeId{ 0 };

        LiveInfo* liveInfo = nullptr;
        RegionInfo* ownerRegion = nullptr; // if unit is SUBORDINATE_UNIT

        RegionInfo* ownerRegion0 = nullptr; // if unit is SUBORDINATE_UNIT

        LiveInfo* retainedLiveInfo = nullptr;
        // Monotonic within a retained-snapshot cycle: only successful
        // Preserve arms it; old-mark start or region-life bump disarms it.
        uint8_t retainedEverPreserved = 0;
        uint64_t retainedLiveInfoEpoch = 0;
        MAddress retainedLiveInfoCoveredUpTo = 0;
        RegionLifeId retainedLifeId = 0;
        // holderlive (F2): per-region-life history of the three fields above. Reset by
        // InitRegionInfo so "preserve count 0" means "never preserved in this life", not
        // "never preserved since boot".
        uint32_t retainedPreserveCnt = 0;
        uint32_t retainedClearCnt = 0;
        uint8_t retainedLastOp = RETAINED_OP_NONE;
        // routedest: 1 while some from-region's published RouteInfo still names this region
        // as a destination. Stamped inside the ROUTING critical section by
        // RouteOrCompactRegionImpl, dropped once per route generation by
        // ClearRouteDestHoldFlags after PrepareFromRegionList's dispel walk has retired every
        // route that could name it. Read by the reclaim entry points, which refuse a held
        // region — the to-side counterpart of ZGC's per-page reference count, expressed as a
        // gate rather than a count because the answer only has to change once per generation.
        //
        // Durability, and the reason this works at all: UnitInfo lives BELOW heapStartAddress
        // (UnitInfo::GetUnitInfo returns heapStartAddress - (idx + 1) * sizeof(UnitInfo)),
        // while ClearUnits MemorySets and ReleaseUnits madvises only payload at
        // heapStartAddress + idx * UNIT_SIZE. A flag in UnitMetadata therefore survives both
        // zeroing writers. Do not "fix" this on the assumption that ClearUnits wipes it.
        //
        // Placement: deliberately here, in the padding after retainedLastOp and before the
        // 8-aligned retainedMarkWords pointer, not beside markFaceSealed where it reads more
        // naturally. Measured: beside markFaceSealed it grew sizeof(UnitInfo) 192 -> 200,
        // and per-unit metadata is per-page (UNIT_SIZE is the system page size), so that is
        // +0.195% of the whole heap for one byte. Here it is free.
        //
        // Plain uint8_t rather than a regionStateBitField slot: bitfield writes are not
        // atomic (see the comment on that union above) and this is written by a routing
        // thread while reclaim threads read it. Same reason notRelocatableThisCycle and
        // markFaceSealed are plain bytes.
        uint8_t routeDestHold = 0;
        // ZForwarding.hpp:66-69. Fits the 6-byte hole after routeDestHold:
        // hold(1)+claimed(1)+done(1)+pad(1)+ref(4) = 8, then retainedMarkWords
        // stays 8-aligned.
        std::atomic<bool> fwdClaimed{ false };
        std::atomic<bool> fwdDone{ false };
        std::atomic<int32_t> fwdRefCount{ 0 };
        // holderlive (F2): owned copy of the retained mark bits (mark | resurrect). Null unless
        // MRT_GCV2_RETAINED_OWN_COPY=1. Freed by ClearLiveInfo / InitRegionInfo.
        uint64_t* retainedMarkWords = nullptr;
        uint32_t retainedMarkWordCnt = 0;
        // In-flight copiers that hold LOCKED (TryLock success → Unlock). Fills the
        // 4-byte hole after retainedMarkWordCnt; sizeof(UnitInfo) stays 208.
        std::atomic<int32_t> copyInflight{ ZForwardingLife::CopyAdmissionSealedWord() };

        // resolveto: Compact packs densely; GetRoute prefix-sum dests are holes.
        // Table maps from-offset → actual dest for COMPACTED regions only.
        void* compactRouteTable = nullptr;

        // ZGC zPage allocate-black: objects at offset >= this allocPtr, snapshotted
        // at ClearLiveInfo / mark-start, are implicitly live (zPage.inline.hpp:180-185
        // is_allocating). 0 = no mark-start yet.
        uintptr_t markStartAllocPtr;
        RouteInfo routeInfo;
        uint64_t snapshotEpoch = 0;
        // used to traverse ghost region.
        uint32_t nextRegionIdx0;

        // the writing operation in C++ Bit-Field feature is not atomic, if we wants to
        // change the value, we must use specific interface implenmented by BitField.
        union {
            struct {
                uint8_t unitRole : BIT_LENGTH;
                uint8_t unitRole0 : BIT_LENGTH; // unit class before forwarded and reclaimed.
            };
            BitField<uint8_t> unitRoleBitField;
        };

        // the writing operation in C++ Bit-Field feature is not atomic, if we wants to
        // change the value, we must use specific interface implenmented by BitField.
        union {
            struct {
                uint8_t regionType : BIT_LENGTH;

                // a region allocated during trace phase, gc should not put any object in this region into satb buffer.
                // the count of objects which can be put into satb buffer should has an upper-bound,
                // so that concurrent tracing can converge and terminate.
                uint8_t isTraceRegion : 1;

                // true if this unit belongs to a ghost region, which is an unreal region for keeping reclaimed
                // from-region. ghost region is set up to memorize a from-region before from-space is forwarded. this
                // flag is cleared when ghost-from-space is cleared. Note this flag is essentially important for
                // FindToVersion().
                uint8_t inGhostFromRegion : 1;
                uint8_t isMarked : 1;
                uint8_t isEnqueued : 1;
                uint8_t isResurrected : 1;
            };
            BitField<uint16_t> regionStateBitField;
        };
        // One atomic snapshot binds state to region life. The exact-start table
        // reuses the old split-field footprint, preserving UnitInfo size.
        std::atomic<uint64_t> routeStateSnapshot{ 0 };
        void* routeStartTable = nullptr;
        RegionLifeId ghostLifeId = 0;
        // twoflags: orthogonal to isTraceRegion.
        // isTraceRegion = implicit-black / ShouldEnqueue skip (cleared by HandleTraceRegions).
        // notRelocatableThisCycle = allocated after mark start this cycle → not a
        // relocation / CSet candidate until next PrepareTrace. Never read by ShouldEnqueue.
        uint8_t notRelocatableThisCycle = 0;
        // sealcheck: 1 after RouteRegion enters ROUTING (geometry face frozen).
        uint8_t markFaceSealed = 0;
        ZGenerationId _generation_id;
        RwLock rwLock;
    };

    class UnitInfo {
    public:
        // propgated from RegionManager
        static uintptr_t heapStartAddress; // the address of the first region space to allocate objects
        static size_t totalUnitCount;
        static MemMap* memoryOwner;
        // gatehot: log2(UNIT_SIZE); UNIT_SIZE is always a power-of-two page size.
        // Hot GetUnitIdxAt uses a shift instead of a runtime / on a non-constant divisor.
        static size_t unitSizeShift;

        constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();

        // gatehot: OOB path used to live in the same function as the hot index math.
        // That forced a full frame (dladdr + FormatLog + stack canary) on every call and
        // blocked inlining into TryGetRegionInfoAt / PlausibleManagedObjectGate.
        // Cold-only: same greppable FATAL text as before (unitzero trail).
        ATTR_NO_INLINE ATTR_COLD static size_t GetUnitIdxAtOOB(uintptr_t allocAddr);

        // Hot path: range check + shift. Must stay tiny enough to inline at every call site.
        ALWAYS_INLINE static size_t GetUnitIdxAt(uintptr_t allocAddr)
        {
            uintptr_t start = heapStartAddress;
            size_t units = totalUnitCount;
            size_t shift = unitSizeShift;
            // UNIT_SIZE == (1 << shift); keep arithmetic identical to
            //   start <= addr < start + units * UNIT_SIZE
            // without loading the UNIT_SIZE global or emitting a DIV.
            if (LIKELY(start <= allocAddr &&
                       ((allocAddr - start) >> shift) < units)) {
                size_t idx = (allocAddr - start) >> shift;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                // Debug builds always cross-check shift vs div (GATEEQUIV math).
                size_t divIdx = (allocAddr - start) / UNIT_SIZE;
                if (UNLIKELY(idx != divIdx)) {
                    LOG(RTLOG_FATAL, "GetUnitIdxAt GATEEQUIV mismatch addr=%#zx shift=%zu div=%zu",
                        allocAddr, idx, divIdx);
                }
#endif
                return idx;
            }
            return GetUnitIdxAtOOB(allocAddr);
        }

        ALWAYS_INLINE static UnitInfo* GetUnitInfoAt(uintptr_t allocAddr)
        {
            return GetUnitInfo(GetUnitIdxAt(allocAddr));
        }

        // get the unit address by index
        static MAddress GetUnitAddress(size_t idx)
        {
            CHECK(idx < totalUnitCount);
            return heapStartAddress + idx * UNIT_SIZE;
        }

        static UnitInfo* GetUnitInfo(size_t idx)
        {
            CHECK(idx < totalUnitCount);
            return reinterpret_cast<UnitInfo*>(heapStartAddress - (idx + 1) * sizeof(UnitInfo));
        }

        static size_t GetUnitIdx(const UnitInfo* unit)
        {
            uintptr_t ptr = reinterpret_cast<uintptr_t>(unit);
            if (ptr < heapStartAddress) {
                return (heapStartAddress - ptr) / sizeof(UnitInfo) - 1;
            }

            LOG(RTLOG_FATAL, "UnitInfo::GetUnitIdx() Should not execute here, abort.");
            return 0;
        }

        UnitInfo() = delete;
        UnitInfo(const UnitInfo&) = delete;
        UnitInfo& operator=(const UnitInfo&) = delete;
        ~UnitInfo() = delete;

        // These interfaces are used to make sure the writing operations of value in C++ Bit Field will be atomic.
        void SetUnitRole(UnitRole role)
        {
            metadata.unitRoleBitField.SetAtomicValue(0, BIT_LENGTH, static_cast<uint8_t>(role));
        }
        void SetUnitRole0(UnitRole role)
        {
            metadata.unitRoleBitField.SetAtomicValue(BIT_LENGTH, BIT_LENGTH, static_cast<uint8_t>(role));
        }
        void SetRegionType(RegionType type)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::REGION_TYPE_FLAG, BIT_LENGTH,
                                                        static_cast<uint8_t>(type));
        }
        void SetTraceRegionFlag(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::TRACE_REGION_FLAG, 1, flag);
        }
        void SetInGhostRegion(uint8_t flag, RegionLifeId life = 0)
        {
            __atomic_store_n(&metadata.ghostLifeId, life, __ATOMIC_RELEASE);
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
            if (flag != 0) {
                RegionLifeClock::Publish(RegionLifeClock::Carrier::GHOST, life);
            }
        }

        void SetOldMarkedRegionFlag(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1, flag);
        }

        void SetEnqueuedRegionFlag(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::ENQUEUED_REGION_FLAG, 1, flag);
        }

        void SetResurrectedRegionFlag(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::RESURRECTED_REGION_FLAG, 1, flag);
        }

        // Publish the owner before the discriminator that guards it, so a reader which observes
        // SUBORDINATE_UNIT always finds a non-null ownerRegion (:530-546). SetUnitRole is an
        // acq_rel compare-exchange (BitField::SetAtomicValue :46-58), which orders the store
        // above it.
        void InitSubordinateUnit(RegionInfo* owner)
        {
            metadata.ownerRegion = owner;
            SetInGhostRegion(0);
            SetUnitRole(UnitRole::SUBORDINATE_UNIT);
        }

        void ToFreeRegion() { InitFreeRegion(GetUnitIdx(this), 1); }

        void ClearUnit() { ClearUnits(GetUnitIdx(this), 1); }

        void ReleaseUnit() { ReleaseUnits(GetUnitIdx(this), 1); }

        UnitMetadata& GetMetadata() { return metadata; }

        UnitRole GetUnitRole() const { return static_cast<UnitRole>(metadata.unitRole); }

        class UnitInfoArray {
        private:
            UnitInfo* unitArray;
            size_t size;
        public:
            UnitInfoArray(UnitInfo* unit, size_t size): size(size)
            {
                uintptr_t lastUnitAddress = reinterpret_cast<uintptr_t>(unit) -
                                            (size - 1) * sizeof(RegionInfo::UnitInfo);
                unitArray = reinterpret_cast<RegionInfo::UnitInfo*>(lastUnitAddress);
            }

            UnitInfo& operator[](size_t index)
            {
                CHECK(index >= 0 && index < size);
                return unitArray[size - index - 1];
            }
        };

    private:
        UnitMetadata metadata;
    };

    // unitRole selects the ownerRegion/liveInfo payload, and its writers publish it with an acq_rel
    // compare-exchange (UnitInfo::InitSubordinateUnit, InitRegionInfo below). Read it with
    // acquire so that the selected payload read which follows in GetRegionInfo/GetRegionInfoAt/
    // GetGhostFromRegionAt cannot be hoisted above the discriminator: a plain pair of loads may
    // be reordered, or folded into an unconditional load plus a select, either of which would
    // defeat the writer's ordering. On x86_64 an acquire load is the same instruction as a
    // relaxed one, so this constrains the compiler and costs nothing at run time.
    static UnitRole LoadUnitRole(UnitInfo* unit)
    {
        return static_cast<UnitRole>(unit->GetMetadata().unitRoleBitField.GetAtomicValue(0, BIT_LENGTH));
    }

    static UnitRole LoadUnitRole0(UnitInfo* unit)
    {
        return static_cast<UnitRole>(
            unit->GetMetadata().unitRoleBitField.GetAtomicValue(BIT_LENGTH, BIT_LENGTH) >> BIT_LENGTH);
    }

    void ObserveLifeBoundary() const
    {
        RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::ROUTE_INFO,
                                                metadata.routeInfo.HasRoute(),
                                                metadata.routeInfo.GetLifeId());
        const uint64_t routeSnapshot = metadata.routeStateSnapshot.load(std::memory_order_acquire);
        RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::ROUTE_STATE,
                                                RouteStateFromSnapshot(routeSnapshot) != RouteState::NORMAL,
                                                RouteLifeFromSnapshot(routeSnapshot));
        RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::GHOST,
                                                metadata.inGhostFromRegion != 0,
                                                metadata.ghostLifeId);
        const ZForwarding::FromPageView* from = GetFromPageView();
        RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::MARK_SNAPSHOT,
                                                from != nullptr, from == nullptr ? 0 : from->lifeId);
        RegionLifeClock::NoteZeroAcrossBoundary(RegionLifeClock::Carrier::RETAINED_COPY,
                                                metadata.retainedLiveInfo != nullptr ||
                                                    metadata.retainedMarkWords != nullptr ||
                                                    metadata.retainedEverPreserved != 0,
                                                metadata.retainedLifeId);
    }

    void BumpRegionLifeId()
    {
        RegionLifeId old = metadata.regionLifeId.load(std::memory_order_relaxed);
        for (;;) {
            if (UNLIKELY(old == std::numeric_limits<RegionLifeId>::max())) {
                LOG(RTLOG_FATAL,
                    "[LIFECLOCK][REGION_LIFE_ID_OVERFLOW] region=%p life=%llu; wraparound is forbidden",
                    this, static_cast<unsigned long long>(old));
                return;
            }
            if (metadata.regionLifeId.compare_exchange_weak(old, old + 1, std::memory_order_release,
                                                            std::memory_order_relaxed)) {
                // A new region life is the hard boundary for the monotonic
                // retained Preserve history.
                metadata.retainedEverPreserved = 0;
                return;
            }
        }
    }

    // unitRole selects between the ownerRegion/liveInfo payloads and allocPtr/regionEnd:
    // a reader that observes SUBORDINATE_UNIT dereferences metadata.ownerRegion (:530-546),
    // and a reader that observes SMALL_SIZED_UNITS or LARGE_SIZED_UNITS treats this unit as a
    // region head and reads metadata.regionEnd (IsValidRegion :1018-1022). This function both
    // leaves the first state and enters the second, and the readers are not stopped by
    // ScopedStopTheWorld -- the collector's own promotion walk (RegionManager.cpp:549-551) runs
    // while the finalizer thread reclaims regions through here. So the role is moved to the
    // neutral FREE_UNITS first, the payload is rewritten, and only then is the real role
    // published. FREE_UNITS is safe to expose at any moment: it makes readers treat the unit as
    // itself, and it is neither a valid region nor a subordinate one.
    // SetUnitRole is an acq_rel compare-exchange (BitField::SetAtomicValue :46-58), so neither
    // bracket can be reordered with the payload stores between them.
    void InitRegionInfo(size_t nUnit, UnitRole uClass)
    {
        CHECK_DETAIL(GetRegionListOwner() == nullptr, "reinitializing a region still owned by a list");
        CHECK_DETAIL(FromPageDetach::FromPageDetachCheck(this, FromPageDetach::Site::INIT_REGION_INFO),
                     "CJRT_FROM_REUSE_GATE bypass reached InitRegionInfo region=%p units=%zu", this, nUnit);
        M0Correlation::InvalidateRegionBindings(GetRegionStart(), GetRegionLifeId());
        SetUnitRole(UnitRole::FREE_UNITS);
        // Invalidate every old-life carrier before clearing any of its payload.
        // Readers either retain the old page (detachgate) or observe this bump and
        // reject the old incarnation; there is no wraparound fallback.
        ObserveLifeBoundary();
        BumpRegionLifeId();
        {
            uint8_t cur = __atomic_load_n(&metadata.routeDestHold, __ATOMIC_RELAXED);
            uint8_t seq = static_cast<uint8_t>(((cur >> 1) + 1) & 0x7f);
            uint8_t next = static_cast<uint8_t>((cur & 1u) | (seq << 1));
            __atomic_store_n(&metadata.routeDestHold, next, __ATOMIC_RELEASE);
        }
        // See DispelGhostFromRegion: retire the route before detaching its compact table.
        SetRouteState(NORMAL);
        ZForwardingLife::ResetIdle(metadata.fwdRefCount, metadata.fwdClaimed, metadata.fwdDone);
        WaitCopiedBeforePayloadWipe(this, "InitRegionInfo");
        ZForwardingLife::reset_copy_sealed(metadata.copyInflight);
        ForwardingTable::ClearEntries(GetRegionStart(), nUnit * RegionInfo::UNIT_SIZE);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        // Unset until ClearLiveInfo starts a mark. 0 so idle / test regions do
        // not treat every object as allocate-black.
        metadata.markStartAllocPtr = 0;
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        // Ghost walk (PrepareFromRegionList) follows nextRegionIdx0. A reused
        // region that still named its previous-life successor kept a retired
        // from-space chain alive across InitRegion (RegionManager.h:782).
        metadata.nextRegionIdx0 = NULLPTR_IDX;
        metadata.regionListOwner.store(nullptr, std::memory_order_relaxed);
        metadata.censusBoundaryOffset = 0;
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        metadata.liveInfo = nullptr;
        ClearCurrentMarkFace();
        FreeCompactRouteTable();
        FreeRouteStartTable();
        FreeRetainedMarkWords();
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
        metadata.retainedLifeId = 0;
        // holderlive (F2): new region life — its predecessor's snapshot history does not
        // describe the objects that are about to be allocated here.
        metadata.retainedPreserveCnt = 0;
        metadata.retainedClearCnt = 0;
        metadata.retainedLastOp = RETAINED_OP_NONE;
        BumpSnapshotEpochFromInitRegion();
        // routedest: this is the reuse edge named in the defect. TakeRegion has already run
        // ClearUnits over this payload; if a published route still names this region, the
        // route now answers into zeroed (or freshly re-allocated) memory. Count it here
        // rather than at ClearUnits because this is the one call that runs exactly once per
        // reuse. The hold is deliberately NOT cleared: reaching this point while held means
        // a reclaim gate was bypassed, and leaving the flag set keeps the region out of the
        // next collection set instead of silently papering over the escape.
        SetRegionType(RegionType::FREE_REGION);
        SetTraceRegionFlag(0);
        SetNotRelocatableThisCycle(0);
        // routedest part D: InitRegionInfo resets liveInfo, liveInfo0, the compact route
        // table and every retained* field, but historically left routeState and routeInfo
        // alone, so a re-taken region inherited its predecessor's plan and its predecessor's
        // COMPACTED state. RouteObject reads `RouteRegion(r) || r->IsCompacted()`
        // (RegionManager.h:605, :626), and the IsCompacted arm bypasses the ghost gate — so
        // an inherited COMPACTED state left a null liveInfo0 as the only thing standing
        // between a reused region and answering a route out of the previous life's geometry.
        // The whole design rests on "the ghost bit bounds route readability"; this is the
        // one hole in that obligation.
        // Route state was reset before FreeCompactRouteTable; clear the geometry here.
        metadata.routeInfo.Clear();
        // Ghost lives in unit metadata, not payload: ClearUnits cannot clear it.
        // TakeRegion reuses garbage without DispelGhostFromRegion (RegionInfo.h:667-698).
        SetInGhostRegion(0);
        SetOldMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
        SetYoungRegionFlag(0);
        SetMarkFaceSealed(false);
        __atomic_store_n(&metadata.rawPointerObjectCount, 0, __ATOMIC_SEQ_CST);
        SetUnitRole(uClass);
    }

    void InitRegion(size_t nUnit, UnitRole uClass)
    {
        InitRegionInfo(nUnit, uClass);



        // initialize region's subordinate units.

        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            array[i].InitSubordinateUnit(this);
        }
        AssertGhostClearedAfterReuse(nUnit);
    }

    static constexpr uint32_t NULLPTR_IDX = UnitInfo::INVALID_IDX;
    UnitMetadata metadata;
};
} // namespace MapleRuntime
#endif // MRT_REGION_INFO_H
