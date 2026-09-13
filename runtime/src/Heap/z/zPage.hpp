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
#include <functional>
#include <limits>
#include <sched.h>
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
#include "Heap/Collector/LiveInfoArena.h"
#include "Heap/z/zForwarding.hpp"
#include "Heap/Collector/GcInfos.h"
#include "Heap/z/zLiveMap.hpp"
#include "Heap/Collector/ManagedObjectGate.h"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zVirtualMemoryManager.hpp"
#include "Heap/z/zGranuleMap.hpp"

#include "Base/TimeUtils.h"
#include "securec.h"
#ifdef CANGJIE_ASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif

#include "Heap/z/zBitField.hpp"
namespace MapleRuntime {
class RegionList;
// this class is the metadata of region, it contains all the information needed to manage its corresponding memory.
// Region memory is composed of several Units, described by UnitInfo.
// sizeof(RegionInfo) must be equal to sizeof(UnitInfo). We rely on this fact to calculate region-related address.


// Metadata ABI: UI(i) is stored below the exported heapStartAddress anchor at
// anchor - (i + 1) * sizeof(UnitInfo). The anchor is the metadata array end;
// payload addresses come from unitSegments, independently of this array.
// region info is stored in the metadata of its primary unit (i.e. the first unit).
class RegionInfo {
    // The table serializes publication/unbinding of this facade's owner.
    friend class ForwardingTable;
public:

    enum class RetainedLiveInfoState : uint8_t {
        NEVER_EXAMINED,
        SNAPSHOT_VALID,
        SNAPSHOT_EMPTY,
        // A retained snapshot was published in this snapshot cycle and its
        // carrier is no longer current.  This is derived from the monotonic
        // ever-preserved bit; clear/unbind exits never write this state.
        SNAPSHOT_LOST,
    };



    unsigned RelocateObserve() const;

    static const size_t UNIT_SIZE; // same as system page size

    // regarding a object as a large object when the size is greater than 8 units.
    static const size_t LARGE_OBJECT_DEFAULT_THRESHOLD;

    // release a large object when the size is greater than 4096KB.
    static constexpr size_t LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB;

    // sealcheck: mark face frozen for geometry (M3). Set at RouteRegion ROUTING entry.
    bool IsMarkFaceSealed() const
    {
        return (__atomic_load_n(&metadata.markFaceSealed, std::memory_order_acquire) & MARK_FACE_SEALED_BIT) != 0;
    }
    void SetMarkFaceSealed(bool v);

    // ZPage::generation()->seqnum(), shared by all pages in that generation.
    uint64_t GetSnapshotEpoch() const;

    uint8_t GetRegionLifeSeq() const
    {
        return static_cast<uint8_t>(__atomic_load_n(&metadata.regionLifeSequence, __ATOMIC_ACQUIRE));
    }

    RegionLifeId GetRegionLifeId() const
    {
        return metadata.regionLifeId.load(std::memory_order_acquire);
    }

    template<Generation G>
    uint64_t GetMarkSnapshotEpoch() const;

    template<Generation G>
    MarkView<G> GetMarkView();

    template<Generation G>
    bool ValidateMarkView(MarkView<G> view) const;


    bool IsCompacted() const;

    bool IsRoutingState();

    // enroltime: when does a region actually join the relocation set?
    //
    // OpenJDK installs the whole set before the colour flips: ZRelocationSet::install runs in the
    // concurrent select_relocation_set (zGeneration.cpp:254), and only afterwards does
    // relocate_start -> flip_relocate_start run (:918 -> :922 -> :651).  Because of that ordering,
    // "painted with the current colour, after the flip" is equivalent to "names an object that
    // will not move this cycle", and ZGC's whole colour-epoch argument rests on that equivalence.
    //
    // Pages join the relocation set when forwarding is installed (zRelocationSet.cpp
    // install). A store-good value painted after relocate-start must already name a
    // to-version or wait for the page worker (zRelocate.cpp:382-415).
    //
    // GCPhase is the cheap witness: PREFORWARD/FORWARD mean the relocate-start flip has run.
    static constexpr bool kEnrolTimeProbe = true;
    static std::atomic<uint64_t>& EnrolBeforeFlip();
    static std::atomic<uint64_t>& EnrolAfterFlip();
    void NoteEnrolPhase();

    RegionInfo();
    static inline RegionInfo* NullRegion();

    LiveInfo* GetLiveInfo();

    LiveInfo* GetLiveInfo() const;

    template<Generation G>
    LiveInfo* GetLiveInfoForView(MarkView<G> view) const;

    ZForwarding* GetFromPageCarrier() const;

    const ZForwarding::FromPageView* GetFromPageView() const
    {
        return ForwardingTable::GetFromPageView(const_cast<RegionInfo*>(this));
    }

    bool HasFromPageMetadata() const;

    // Probe-only compatibility surface. The storage is no longer a second
    // current face; it belongs to the immutable from-page metadata carrier.
    LiveInfo* GetLiveInfo0ForProbe() const;

    Generation GetRouteMarkGeneration() const;

    template<Generation G>
    MarkView<G> GetRouteMarkView();

    template<Generation G>
    uint64_t GetMarkEpoch(MarkView<G> view, LiveInfo* liveInfo) const;

    template<Generation G>
    RegionBitmap* GetMarkBitmap(MarkView<G> view, LiveInfo* liveInfo) const;

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, LiveInfo* liveInfo, size_t offset) const;

    bool FromPageAllocatedAfterMarkStart(size_t offset) const;

    bool HasFromPageMarkStartAllocGap() const;

    template<Generation G>
    bool IsFromPageSurvivedObject(MarkView<G> view, size_t offset) const;

    bool IsRouteSurvivedObject(size_t offset);

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

    bool IsRouteMarkedObject(size_t offset);

    bool IsRouteMarkedObject(const BaseObject* object)
    {
        return IsRouteMarkedObject(GetAddressOffset(reinterpret_cast<MAddress>(object)));
    }

    bool IsRouteKnownEmpty();

    RegionBitmap* GetRouteMarkBitmap(LiveInfo* face = nullptr);

    uint64_t GetRouteMarkEpoch(LiveInfo* face);

    uint64_t GetRouteMarkSnapshotEpoch() const;

    size_t RecomputeRouteBitmapLiveBytes(LiveInfo* face);

    size_t GetRouteBitmapLiveBytes(LiveInfo* face);

    // installdomain: if PrepareForwardable snapshotted a null liveInfo, GetRoute always
    // rejects. After MarkObject created current liveInfo, bind it as ghost while still
    // FORWARDABLE so the paint is route-visible (pointer-share, same as PrepareForwardable).
    void BindLiveInfo0FromLiveIfNull();

    bool IsRetainedLifeCurrent() const;

    LiveInfo* GetRetainedLiveInfo() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfo : nullptr;
    }

    RetainedLiveInfoState GetRetainedLiveInfoState() const;

    bool HasEverPreservedRetainedLiveInfo() const { return metadata.retainedEverPreserved != 0; }

    uint64_t GetRetainedLiveInfoEpoch() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfoEpoch : 0;
    }

    MAddress GetRetainedLiveInfoCoveredUpTo() const
    {
        return IsRetainedLifeCurrent() ? metadata.retainedLiveInfoCoveredUpTo : 0;
    }

    void StampRetainedSnapshot();

    // holderlive (F2): the retained snapshot has to answer "was this holder live at the last
    // mark" during every minor until the next major re-marks the region. It cannot do that as a
    // borrowed LiveInfo* whose lifetime is shorter than the retained snapshot.
    // Measured: 100% of remset holders read NEVER_EXAMINED, and for 2113/2115 of them the last
    // thing that touched the snapshot was that unbind ([RETLIVE][why-never] lastOp=clrChecked).
    // So keep our own copy of the bits — regionSize/512 bytes, allocated only for regions that
    // are actually preserved. ZGC keeps the page livemap valid through relocation
    // (zLiveMap.inline.hpp:38-40,86-90); this copy is the equivalent persistent carrier.
    static constexpr bool RetainedOwnCopyEnabled() { return true; }

    // Copy the page's one ordinary livemap plus resurrection bits into the
    // retained owner. No generation-dependent face union is needed.
    void CaptureRetainedMarkWords(LiveInfo* liveInfo, uint64_t epoch, uint8_t largeMarked);

    bool HasRetainedMarkWords() const
    {
        return IsRetainedLifeCurrent() && metadata.retainedMarkWords != nullptr;
    }

    // Same indexing as RegionBitmap::IsMarked.
    bool RetainedMarkWordsSay(size_t offset) const;

    void FreeRetainedMarkWords();




    // A Preserve attempt replaces the previous publication.  Keep the
    // monotonic history armed, but invalidate the carrier until this attempt
    // proves that it has a snapshot and publishes it in NoteRetainedPreserve.
    ALWAYS_INLINE void BeginRetainedPreserve()
    {
        __atomic_store_n(&metadata.retainedLifeId, static_cast<RegionLifeId>(0), __ATOMIC_RELEASE);
    }

    void PreserveRetainedLiveInfo();

    MAddress GetCensusBoundary() const
    {
        return GetRegionStart() + metadata.censusBoundaryOffset;
    }

    void StampCensusBoundary();

    void ResetCensusBoundary() { metadata.censusBoundaryOffset = 0; }

    void PreserveRetainedLiveInfoUpTo(MAddress boundary);

    ALWAYS_INLINE void PreserveRetainedLiveInfo(MAddress coveredUpToOverride);

    // holderlive (F2): record the outcome of a Preserve* attempt. Only a
    // successful publication arms the monotonic bit and carrier stamp.
    ALWAYS_INLINE void NoteRetainedPreserve(bool succeeded);


    bool IsRetainedSnapshotValid() const;

    // ZPage constructs its livemap before publishing the page in the page table.
    void InitializeLiveInfo();

    template<Generation G>
    RegionBitmap* GetMarkBitmap(MarkView<G> view);

    template<Generation G>
    RegionBitmap* GetOrAllocMarkBitmap(MarkView<G> view);

    RegionBitmap* GetResurrectBitmap();

    RegionBitmap* GetEnqueueBitmap();

    template<Generation G>
    uint8_t GetMarkedRegionFlag(MarkView<G> view) const;

    template<Generation G>
    void SetMarkedRegionFlag(MarkView<G> view, uint8_t flag);

    void ResetMarkBit(MarkView<Generation::Old> view);

    Generation GetOwnerGeneration() const;

    template<Generation G>
    bool MarkFaceMatchesOwner() const
    {
        return GetOwnerGeneration() == G;
    }






    template<Generation G>
    void VerifyMarkFaceOwner(const BaseObject* obj, const char* site) const;


    // livesame / ZGC zMark.inline.hpp + zBitMap.inline.hpp:inc_live — count only on 0→1.
    // MarkBits returns true if already marked; false on first paint. AddLive only then.
    template<Generation G>
    bool MarkLargeObject(MarkView<G> view, const BaseObject* obj, size_t size, bool accountLive, bool& firstLive);

    template<Generation G>
    bool MarkObject(MarkView<G> view, const BaseObject* obj);

    template<Generation G>
    bool MarkObject(MarkView<G> view, const BaseObject* obj, size_t objSize, bool accountLive = true);

    // ZGC zMark.cpp:405-418: the mark transition and first-live ownership
    // are separate results. A finalizable-to-strong upgrade only owns the
    // former; deferred accounting must carry the latter out of the pair RMW.
    template<Generation G>
    bool MarkObjectWithLiveClaim(MarkView<G> view, const BaseObject* obj, size_t objSize,
                                 bool accountLive, bool& firstLive);

    bool MarkObjectByOwner(const BaseObject* obj);

    bool MarkObjectByOwner(const BaseObject* obj, size_t objSize, bool accountLive = true);

    bool MarkObjectByOwnerWithLiveClaim(const BaseObject* obj, size_t objSize,
                                        bool accountLive, bool& firstLive);

    bool ResurrectObject(const BaseObject* obj, size_t offset);

    bool ResurrectObjectWithLiveClaim(const BaseObject* obj, size_t offset,
                                     bool accountLive, bool& firstLive);

    bool EnqueueObject(const BaseObject* obj, size_t offset);

    bool IsResurrectedObject(const BaseObject* obj);

    bool IsResurrectedObject(size_t offset);



    // cjpmnull2: ZGC empty = this-cycle marked ∧ live==0. Epoch mismatch / no face
    // is "not marked this cycle", not empty (zPage.inline.hpp:223-225).
    static std::atomic<size_t> ikeTrueEmpty;
    static std::atomic<size_t> ikeConservativeKeep;
    static std::atomic<size_t> ikeConservativeKeepBytes;
    static std::atomic<size_t> ikeNullFaceKeep;
    static std::atomic<size_t> ikeEpochKeep;
    static std::atomic<bool> ikeAtexitInstalled;



    // Returns false if face is stale (counts as unmarked). true ⇒ epoch matches; caller checks bits.
    template<Generation G>
    bool NoteMarkEpochOnRead(MarkView<G> view, LiveInfo* liveInfo);

    template<Generation G>
    bool IsMarkedObject(MarkView<G> view, const BaseObject* obj);

    template<Generation G>
    bool IsMarkedObject(MarkView<G> view, size_t offset);

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, size_t offset);

    bool IsEnqueuedObject(size_t offset);

    ALWAYS_INLINE size_t GetAddressOffset(MAddress address);

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

    // The reverse metadata array remains an ABI adapter. Its anchor need not
    // be adjacent to payload reservations. Cache indices are dense within each
    // segment, with an unused index between segments to prevent coalescing.
    struct UnitSegment {
        MemoryRange range;
        size_t firstIndex;
    };

    static std::vector<UnitSegment> unitSegments;
    static ZGranuleMap<RegionInfo*> pageOwners;

    // zSafeDelete.inline.hpp:46-59 / zArray.inline.hpp:210-246. The ABI
    // stores descriptors in a fixed array, so defer descriptor reinitialization
    // and cache hand-back instead of deleting a separately allocated ZPage.
    static std::mutex pageRetirementMutex;
    static size_t pageIterationCount;
    static std::vector<std::function<void()>> deferredPageRetirements;

    class PageIterationScope {
    public:
        PageIterationScope()
        {
            std::lock_guard<std::mutex> lock(pageRetirementMutex);
            ++pageIterationCount;
        }

        ~PageIterationScope()
        {
            std::vector<std::function<void()>> retired;
            {
                std::lock_guard<std::mutex> lock(pageRetirementMutex);
                CHECK(pageIterationCount != 0);
                if (--pageIterationCount == 0) {
                    retired.swap(deferredPageRetirements);
                }
            }
            // Run existing allocator paths outside the activation lock, as
            // ZActivatedArray::deactivate_and_apply does.
            for (auto& retire : retired) {
                retire();
            }
        }

        PageIterationScope(const PageIterationScope&) = delete;
        PageIterationScope& operator=(const PageIterationScope&) = delete;
    };

    static void RetirePage(RegionInfo* region, std::function<void()> retire);

    static size_t IndexedUnitCount(const std::vector<MemoryRange>& ranges);

    static void Initialize(size_t nUnit, uintptr_t heapAddress, MemMap* memoryOwner = nullptr)
    {
        InitializeSegments(heapAddress, { MemoryRange{ heapAddress, nUnit * UNIT_SIZE } }, memoryOwner);
    }

    static void InitializeSegments(uintptr_t metadataEnd, const std::vector<MemoryRange>& ranges,
                                   MemMap* memoryOwner);

    static size_t FindUnitIndex(uintptr_t address);

    static bool ContainsUnitRange(uintptr_t start, size_t size);

    static void VisitPageOwners(const std::function<void(RegionInfo*)>& visitor);

    static RegionInfo* GetRegionInfo(uint32_t idx)
    {
        return TryGetRegionInfoAt(GetUnitAddress(idx));
    }

    // Safely query a heap address whose unit may no longer have a live owning region.
    ALWAYS_INLINE static RegionInfo* TryGetRegionInfoAt(uintptr_t allocAddr);

    // The caller must know that allocAddr resolves to an extant region owner.
    static RegionInfo* GetRegionInfoAt(uintptr_t allocAddr);

    static bool InGhostFromRegion(BaseObject* obj)
    {
        return GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj)) != nullptr;
    }

    static RegionInfo* GetGhostFromRegionAt(uintptr_t allocAddr);

#if defined(MRT_GC_UNIT_TESTS)
    using GhostLookupTestHook = void (*)(RegionInfo*);
    MRT_EXPORT static void SetGhostLookupTestHook(GhostLookupTestHook hook);
    MRT_EXPORT static size_t GhostLookupTestHookCalls();


#endif

    static void InitFreeRegion(size_t unitIdx, size_t nUnit);

    static RegionInfo* InitRegion(size_t unitIdx, size_t nUnit, RegionInfo::UnitRole uclass);

    static RegionInfo* InitRegionAt(uintptr_t addr, size_t nUnit, RegionInfo::UnitRole uclass);

    static MAddress GetUnitAddress(size_t unitIdx) { return UnitInfo::GetUnitAddress(unitIdx); }

    static void WaitCopiedBeforePayloadWipe(RegionInfo* region, const char* site);

    static void ClearUnits(size_t idx, size_t cnt);

    static size_t CommitUnits(size_t idx, size_t cnt);

    static size_t GetCommittedCapacity()
    {
        return UnitInfo::memoryOwner == nullptr ? 0 : UnitInfo::memoryOwner->GetCommittedSize();
    }

    static size_t GetCommittedUnitBytes(size_t idx, size_t cnt);

    static void ReleaseUnits(size_t idx, size_t cnt);

    static size_t ReleaseUnitsPartial(size_t idx, size_t cnt)
    {
        return ReleaseUnitsPartialImpl(idx, cnt, false);
    }

    static size_t ReleaseUnitsDeferred(size_t idx, size_t cnt)
    {
        return ReleaseUnitsPartialImpl(idx, cnt, true);
    }

    static size_t PublishUnitsRelease(size_t idx, size_t completed);

    static size_t ReleaseUnitsPartialImpl(size_t idx, size_t cnt, bool deferred);

    BaseObject* GetFirstObject() const { return from_region_addr(GetRegionStart()); }

    bool IsEmpty() const;

    size_t GetRegionSize() const;

    // Read-only, defensive extent for the phase-1 detach census. InitRegionInfo
    // calls the census before metadata.regionEnd is installed on a never-used
    // unit, so that case is one unit rather than an underflowed stale extent.
    size_t GetRegionSizeForDetachCheck() const;

    size_t GetUnitCount() const { return GetRegionSize() / UNIT_SIZE; }

    size_t GetGhostRegionSize() const;

    size_t GetGhostRegionUnitCount() const { return GetGhostRegionSize() / UNIT_SIZE; }

    size_t GetAvailableSize() const;

    size_t GetRegionAllocatedSize() const { return GetRegionAllocPtr() - GetRegionStart(); }

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpRegionInfo(LogType type) const;
    const char* GetTypeName() const;
#endif

    void VisitAllObjects(const std::function<void(BaseObject*)>&& func);
    bool VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func);

    // zRememberedSet.cpp:144-152 / zLiveMap.inline.hpp:181-221 find_base:
    // nearest object-start pair (strong or finalizable) at or before a field.
    RegionBitmap* GetLiveStartBitmap();

    MAddress FindLiveObjectStart(MAddress field);

    bool fromPageLargeMarked();

    void CollectLiveObjectStarts(std::vector<MAddress>& out);

    // After-copy Exempt parks FORWARDED residuals (zRelocate.cpp:1041-1047).
    // CSet empty-select still needs those headers; strip only at the next install,
    // after the table is retired (zRelocationSet.cpp:91-96). A leftover FORWARDED
    // with no table entry makes ForwardObjectImpl recopy rather than return dest
    // (si_addr=0x8 / near-golden drift). Does not touch LOCKED (live copier).
    void ClearRelocationResiduals();

    // reset so that this region can be reused for allocation
    void InitFreeUnits();



    bool IsCompactRouteDestination(MAddress address) const;

    ZGenerationId generation_id() const;

    template<Generation G>
    void PublishFromPageMetadata(MarkView<G> view);

    // Product publication edge shared by forwarding and from-page liveness.
    // Keep this in the ordinary product inline path: the operation is part of
    // PrepareForwardableRegion, not a test-facing ABI surface.
    template<Generation G>
    __attribute__((always_inline)) inline void PublishForwardingCarrier(MarkView<G> view);

    template<Generation G>
    void PrepareForwardableRegion(MarkView<G> view);

    void ClearGhostRegionBit();

    // dispel all units of this region.
    // inGhostFromRegion is the unique guard condition.

    // T-D guardian (MINOR_CONCURRENCY_0805 §八): parallel windows assert this is frozen.
    // Public for reffix parallel window assert + positive-control inject.
    static std::atomic<size_t> dispelGhostCount;
#if defined(MRT_GC_UNIT_TESTS)
    static std::atomic<GhostLookupTestHook> ghostLookupTestHook;
    static std::atomic<size_t> ghostLookupTestHookCalls;
    static void RunGhostLookupTestHook(RegionInfo* region);
#endif

    static size_t GetDispelGhostCount()
    {
        return dispelGhostCount.load(std::memory_order_relaxed);
    }


    void ClearGhostFromRegionBits();

    void DispelGhostFromRegion();

    bool IsGhostFromRegion() const;

    // After TakeRegion re-init, every unit must have ghost cleared (payload wipe does not touch metadata).
    void AssertGhostClearedAfterReuse(size_t nUnit) const;


    template<Generation G>
    void ClearLiveInfo(MarkView<G> view);


    // ZForwarding::retain_page (zForwarding.cpp:86-108). Three-state: 0 refuses,
    // <0 waits for done then refuses, >0 CAS +1.
    bool RetainForwarding();

    void ReleaseForwarding();

    // ZForwarding::retain_page: the three-state count is the gate, not the list
    // type. After ForwardRegion, CollectRegion moves the region to garbage
    // while the payload is still live; mutator relocate must still pin it.
    bool TryLockReadFromRegion() { return RetainForwarding(); }

    void UnlockReadFromRegion() { ReleaseForwarding(); }

    // RAII retain_page / release_page. ok() is false when the page is already
    // released or claimed — the late reader must not touch from-side state.
    class RetainScope {
    public:
        explicit RetainScope(RegionInfo* region) : RetainScope(ForwardingTable::RetainPageOwner(region)) {}
        explicit RetainScope(ForwardingTable::Owner forwarding)
            : owner(std::move(forwarding)), region(owner ? owner->page() : nullptr),
              retained(owner && owner->retain_page())
        {
            CHECK(!retained || owner->page_life_current());
        }
        ~RetainScope() { Release(); }
        void Release()
        {
            if (retained) {
                owner->release_page();
                retained = false;
            }
        }
        bool ok() const { return retained; }
        bool covers(RegionInfo* page) const { return retained && region == page; }
        ZForwarding* forwarding() const { return owner.get(); }
        ForwardingTable::Owner HoldForwarding() const { return owner; }

        RetainScope(const RetainScope&) = delete;
        RetainScope& operator=(const RetainScope&) = delete;
        RetainScope(RetainScope&&) = delete;
        RetainScope& operator=(RetainScope&&) = delete;

    private:
        ForwardingTable::Owner owner;
        RegionInfo* region;
        bool retained;
    };

    bool ClaimForwarding();

    void MarkForwardingDone();

    bool IsForwardingDone() const;

    bool IsForwardingFaceCurrent() const;

    static constexpr uint32_t FORWARDING_FACE_RESET_BIT = (1U << 31);

    bool IsForwardingFaceReset() const;

    void SetForwardingFaceReset()
    {
        (void)__atomic_fetch_or(&metadata.retainedPreserveCnt, FORWARDING_FACE_RESET_BIT, __ATOMIC_ACQ_REL);
    }

    void ClearForwardingFaceReset()
    {
        (void)__atomic_fetch_and(&metadata.retainedPreserveCnt, ~FORWARDING_FACE_RESET_BIT, __ATOMIC_ACQ_REL);
    }

    void ClearCurrentMarkFace();

    bool IsCurrentFacePublished() const;

    // ZGC has no terminal kept: a page not selected this cycle is an ordinary
    // candidate next cycle (zRelocationSetSelector.cpp:114-196 rebuilds from
    // the page table; zGeneration.cpp:205-213). Drop the in-cycle publish so
    // Next cycle must not treat last cycle's in-place done as this cycle's done.
    ZForwarding* PeekForwardingOwner() const
    {
        return metadata.fwdOwner.load(std::memory_order_acquire);
    }

    int32_t CopyInflightWord() const
    {
        return metadata.copyInflight.load(std::memory_order_acquire);
    }

    int32_t ForwardingRefCount() const;

    bool ForwardingClaimed() const;

    void LockWriteRegion() { metadata.rwLock.LockWrite(); }

    void UnlockWriteRegion() { metadata.rwLock.UnlockWrite(); }

    // zForwarding.cpp:110-181 in_place_relocation_claim_page + detach_page.
    class InPlaceClaimScope {
    public:
        MRT_EXPORT InPlaceClaimScope(RegionInfo* region, ZForwardingLife::Retire site);

        ~InPlaceClaimScope()
        {
            if (!retiring) return;
            owner->release_page();
            if (ZForwardingLife::CurrentPageWork() != owner.get()) owner->mark_done();
        }

        InPlaceClaimScope(const InPlaceClaimScope&) = delete;
        InPlaceClaimScope& operator=(const InPlaceClaimScope&) = delete;
        InPlaceClaimScope(InPlaceClaimScope&&) = delete;
        InPlaceClaimScope& operator=(InPlaceClaimScope&&) = delete;

    private:
        ForwardingTable::Owner owner;
        bool retiring{ false };
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
    void SetRegionType(RegionType type);
    void SetTraceRegionFlag(uint8_t flag);
    // twoflags: CSet/route exclusion only. Independent of isTraceRegion lifetime.
    void SetNotRelocatableThisCycle(uint8_t flag)
    {
        __atomic_store_n(&metadata.notRelocatableThisCycle, flag, __ATOMIC_RELEASE);
    }
    bool IsNotRelocatableThisCycle() const
    {
        return __atomic_load_n(&metadata.notRelocatableThisCycle, __ATOMIC_ACQUIRE) != 0;
    }
    void SetInGhostRegion(uint8_t flag);

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

    bool IsYoungRegion() const;

    static size_t GetYoungRegionCount();

    static bool HasYoungRegions();

    // Promotion replaces current page metadata instead of retargeting the same
    // liveness object. The old Young metadata remains available only through the
    // from-page carrier; the new Old current metadata starts with no livemap.
    MarkView<Generation::Old> PromoteYoungRegion(MarkView<Generation::Young> youngView);

    void SetYoungAge(uint8_t age);

    uint8_t GetYoungAge() const;

    RegionType GetRegionType() const;
    UnitRole GetUnitRole() const { return static_cast<UnitRole>(metadata.unitRole); }

    size_t GetUnitIdx() const { return RegionInfo::UnitInfo::GetUnitIdx(reinterpret_cast<const UnitInfo*>(this)); }

    MAddress GetRegionStart() const;

    MAddress GetRegionEnd() const;

    void SetRegionAllocPtr(MAddress addr) { metadata.allocPtr = addr; }

    MAddress GetRegionAllocPtr() const;

    MAddress GetMarkStartAllocPtr() const { return metadata.markStartAllocPtr; }

    // offset ≥ mark-start allocPtr (exclusive end at ClearLiveInfo). Objects
    // bumped after that point are ZGC allocate-black / is_allocating.
    // water == start means the region was empty at mark-start, so every
    // object now in it was born after that snapshot.
    bool AllocatedAfterMarkStart(size_t offset) const;

    bool HasMarkStartAllocGap() const;

    int32_t IncRawPointerObjectCount();

    int32_t DecRawPointerObjectCount();

    int32_t GetRawPointerObjectCount() const
    {
        return __atomic_load_n(&metadata.rawPointerObjectCount, __ATOMIC_SEQ_CST);
    }

    bool CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal);

    uintptr_t Alloc(size_t size);

    // for regions shared by multithreads
    uintptr_t AtomicAlloc(size_t size);

    // zHeap.cpp:298-311 undo_alloc_object_for_relocation / zPage undo_alloc_object_atomic:
    // rewind allocPtr only if this object is still the bump tip. Failure is allowed.
    bool UndoAllocObjectAtomic(uintptr_t addr, size_t size);

    bool IsTraceRegion() const { return metadata.isTraceRegion == 1; }

    // copyable during concurrent copying gc.
    bool IsSmallRegion() const;

    bool IsLargeRegion() const;

    bool IsThreadLocalRegion() const
    {
        return static_cast<RegionType>(metadata.regionType) == RegionType::THREAD_LOCAL_REGION;
    }

    bool IsPinnedRegion() const;

    RegionInfo* GetPrevRegion() const;

    // Intrusive-list authority. A region has at most one owning RegionList;
    // ghost snapshots intentionally do not modify this token.
    RegionList* GetRegionListOwner() const { return metadata.regionListOwner.load(std::memory_order_acquire); }

    void SetRegionListOwner(RegionList* owner) { metadata.regionListOwner.store(owner, std::memory_order_release); }

    void SetPrevRegion(const RegionInfo* r);

    RegionInfo* GetNextRegion() const;

    RegionInfo* GetNextGhostRegion() const;

    void SetNextRegion(const RegionInfo* r);

    bool IsFromRegion() const { return GetRegionType() == RegionType::FROM_REGION; }
    bool IsLoneFromRegion() const { return GetRegionType() == RegionType::LONE_FROM_REGION; }
    bool IsUnmovableFromRegion() const;

    bool IsToRegion() const { return GetRegionType() == RegionType::TO_REGION; }

    bool IsGarbageRegion() const { return GetRegionType() == RegionType::GARBAGE_REGION; }
    bool IsFreeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::FREE_UNITS; }

    bool IsValidRegion() const;

    // ZPage::live_bytes/live_objects use the page's single livemap. A page
    // not touched in this generation sequence has no published marking data.
    RegionBitmap* GetCurrentLiveMap() const;

    uint64_t GetLiveByteCount() const;

    uint32_t GetLiveObjectCount() const;

    // ZPage::inc_live: the mark winner or its worker cache owns this addition.
    void AddLiveCounts(uint32_t objects, uint64_t bytes);

    bool IsLiveCountAuthoritative() const
    {
        return IsCurrentFacePublished();
    }

    // ZGC zGeneration.cpp:216-221 / zPage.inline.hpp:223-225:
    //   is_marked = livemap.seqnum == generation.seqnum
    //   register_empty_page iff !is_marked — but that is safe only because ZGC's mark
    //   is complete for every relocatable page. Ours is not (GetRouteMarkView mints
    //   epoch from liveInfo0; stale_read viewEpoch≠snapshotEpoch).
    // cjpmnull2: empty = this-cycle marked ∧ live==0. Epoch mismatch / null face
    // means "not marked this cycle", not "empty". Authority still required so a
    // minor cannot reclaim non-young on a bare zero.
    bool IsKnownEmpty(MarkView<Generation::Old> view) const;

    bool IsKnownYoungEmpty(MarkView<Generation::Young> view) const;

    bool IsSafeKnownEmpty(MarkView<Generation::Old> view);

    bool IsSafeKnownYoungEmpty(MarkView<Generation::Young> view);

    // ZForwarding::in_place_relocation_finish drops the completed from-page
    // livemap. The next first mark resets counts before publishing its seqnum.
    template<Generation G>
    void ResetLiveMapAfterForward(MarkView<G> view);

    void RemoveFromList();

private:


    ALWAYS_INLINE void CheckObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const;

    NO_RETURN ATTR_COLD ATTR_NO_INLINE void ReportInvalidObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const;


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
        // First-paint publication state; reset by InitRegionInfo.
        // Only FORWARDING_FACE_RESET_BIT is used.
        uint32_t retainedPreserveCnt = 0;
        uint8_t regionLifeSequence = 0;
        // Borrow the immutable forwarding identity. Its owner reference is
        // released at the page lifecycle boundary, never reset in place.
        std::atomic<ZForwarding*> fwdOwner{ nullptr };
        // Owned retained mark bits (mark | resurrect).
        // Freed by ClearLiveInfo / InitRegionInfo.
        uint64_t* retainedMarkWords = nullptr;
        uint32_t retainedMarkWordCnt = 0;
        // In-flight copiers that hold LOCKED (TryLock success → Unlock). Fills the
        // 4-byte hole after retainedMarkWordCnt; sizeof(UnitInfo) stays 208.
        std::atomic<int32_t> copyInflight{ 0 };

        // resolveto: Compact packs densely; GetRoute prefix-sum dests are holes.
        // Table maps from-offset → actual dest for COMPACTED regions only.

        // ZGC zPage allocate-black: objects at offset >= this allocPtr, snapshotted
        // at ClearLiveInfo / mark-start, are implicitly live (zPage.inline.hpp:180-185
        // is_allocating). 0 = no mark-start yet.
        uintptr_t markStartAllocPtr;
        alignas(8) char routeInfoPad[24]{};
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
        static uintptr_t heapStartAddress; // exported ABI anchor: end of the reverse metadata array
        static size_t totalUnitCount;
        static MemMap* memoryOwner;
        constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();

        ALWAYS_INLINE static size_t GetUnitIdxAt(uintptr_t allocAddr)
        {
            const size_t idx = FindUnitIndex(allocAddr);
            CHECK_DETAIL(idx != INVALID_IDX, "address is outside heap reservations: %#zx", allocAddr);
            return idx;
        }

        ALWAYS_INLINE static UnitInfo* GetUnitInfoAt(uintptr_t allocAddr)
        {
            return GetUnitInfo(GetUnitIdxAt(allocAddr));
        }

        // get the unit address by index
        static MAddress GetUnitAddress(size_t idx)
        {
            CHECK(idx < totalUnitCount);
            for (const auto& segment : unitSegments) {
                if (idx >= segment.firstIndex && idx - segment.firstIndex < segment.range.size / UNIT_SIZE) {
                    return segment.range.start + (idx - segment.firstIndex) * UNIT_SIZE;
                }
            }
            LOG(RTLOG_FATAL, "unit index denotes a reservation boundary: %zu", idx);
            return 0;
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
                const size_t distance = heapStartAddress - ptr;
                if (distance % sizeof(UnitInfo) == 0 && distance / sizeof(UnitInfo) <= totalUnitCount) {
                    return distance / sizeof(UnitInfo) - 1;
                }
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

    // The metadata role remains an ABI/ghost-lifetime discriminator. Current
    // page ownership is published and read through pageOwners, independently
    // of subordinate metadata placement.
    static UnitRole LoadUnitRole(UnitInfo* unit)
    {
        return static_cast<UnitRole>(unit->GetMetadata().unitRoleBitField.GetAtomicValue(0, BIT_LENGTH));
    }

    static UnitRole LoadUnitRole0(UnitInfo* unit);

    void BumpRegionLifeId();

    // Reinitialization consumes an already retired descriptor. The allocator
    // must remove the old page and finish safe retirement before reaching here.
    void InitRegionInfo(size_t nUnit, UnitRole uClass);

    void InitRegion(size_t nUnit, UnitRole uClass);

    static constexpr uint32_t NULLPTR_IDX = UnitInfo::INVALID_IDX;
    UnitMetadata metadata;
};
} // namespace MapleRuntime

#include "Heap/z/zPage.inline.hpp"
#endif // MRT_REGION_INFO_H
