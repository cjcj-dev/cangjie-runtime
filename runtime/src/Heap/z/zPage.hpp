// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_INFO_H
#define MRT_REGION_INFO_H

#include "Heap/z/zPageAge.hpp"

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
#include "Heap/z/zForwarding.hpp"
#include "Heap/Collector/GcInfos.h"
#include "Heap/z/zSafeDelete.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zVirtualMemory.hpp"
#include "Heap/z/zGranuleMap.hpp"

#include "Base/TimeUtils.h"
#include "securec.h"
#ifdef CANGJIE_ASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif

#include "Heap/z/zLiveMap.hpp"
namespace MapleRuntime {
class RegionList;

// Descriptor incarnation id used by the forwarding carrier / ghost walk
// (page-descriptor package retires it with the reused slot).
using RegionLifeId = uint64_t;

// Atomic accessor for the C++ bit fields packed in UnitMetadata (unitRole /
// regionState words). This is a RegionInfo state-word helper, not the ZGC
// ZBitField encode/decode template (zBitField.hpp).
template<typename T>
class AtomicBitField {
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


// Metadata ABI: UI(i) is stored below the exported heapStartAddress anchor at
// anchor - (i + 1) * sizeof(UnitInfo). The anchor is the metadata array end;
// payload addresses come from unitSegments, independently of this array.
// region info is stored in the metadata of its primary unit (i.e. the first unit).
class RegionInfo {
    // The table serializes publication/unbinding of this facade's owner.
    friend class ForwardingTable;
public:

    unsigned RelocateObserve() const;

    static const size_t UNIT_SIZE; // same as system page size

    // regarding a object as a large object when the size is greater than 8 units.
    static const size_t LARGE_OBJECT_DEFAULT_THRESHOLD;

    // release a large object when the size is greater than 4096KB.
    static constexpr size_t LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB;

    // ZPage::generation()->seqnum(), shared by all pages in that generation.
    uint64_t GetSnapshotEpoch() const;
    void ResetPageSequence();
    uint64_t BirthSequence() const { return __atomic_load_n(&metadata.birthSequence, __ATOMIC_ACQUIRE); }
    uint64_t OtherSequence() const { return __atomic_load_n(&metadata.otherSequence, __ATOMIC_ACQUIRE); }
    bool IsAllocating() const;
    bool IsRelocatable() const;

    uint8_t GetRegionLifeSeq() const
    {
        return static_cast<uint8_t>(__atomic_load_n(&metadata.regionLifeSequence, __ATOMIC_ACQUIRE));
    }

    RegionLifeId GetRegionLifeId() const
    {
        return metadata.regionLifeId.load(std::memory_order_acquire);
    }

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
    static RegionInfo* NullRegion();

    // ZPage::_livemap (zPage.hpp:52). One ZLiveMap per page life, owned by
    // this descriptor and constructed with object_max_count() at InitRegionInfo.
    // It is held by pointer only because RegionInfo is a reused slot of the
    // reverse metadata array; the independent page descriptor (zPage.cpp:33-62)
    // embeds it by value.
    ZLiveMap* livemap() const;

    ZForwarding* GetFromPageCarrier() const;

    const ZForwarding::FromPageView* GetFromPageView() const
    {
        return ForwardingTable::GetFromPageView(const_cast<RegionInfo*>(this));
    }

    bool HasFromPageMetadata() const;

    // The original page livemap retained by the forwarding carrier while this
    // slot already describes the reused/promoted page (zForwarding.hpp:44-110
    // holds the whole from ZPage; only its livemap is carried here).
    ZLiveMap* FromPageLiveMap() const;

    Generation GetRouteMarkGeneration() const;

    bool IsFromPageAllocating() const;

    bool IsFromPageSurvivedObject(size_t offset) const;

    bool IsRouteSurvivedObject(size_t offset);

    // Compatibility name for existing relocation callers. Both route and compact
    // now consume the one owner stored in the from-page metadata carrier.
    bool IsOwnerSurvivedObject(size_t offset)
    {
        return IsRouteSurvivedObject(offset);
    }

    bool IsOwnerKnownEmpty()
    {
        return IsRouteKnownEmpty();
    }

    bool IsRouteMarkedObject(size_t offset);

    bool IsRouteMarkedObject(const BaseObject* object)
    {
        return IsRouteMarkedObject(GetAddressOffset(reinterpret_cast<MAddress>(object)));
    }

    bool IsRouteKnownEmpty();

    // installdomain: if PrepareForwardable snapshotted a null livemap, GetRoute always
    // rejects. After mark_object created the current livemap, bind it as ghost while still
    // FORWARDABLE so the paint is route-visible (pointer-share, same as PrepareForwardable).
    void BindFromPageLiveMapIfNull();

    MAddress GetCensusBoundary() const
    {
        return GetRegionStart() + metadata.censusBoundaryOffset;
    }

    void StampCensusBoundary();
    void ResetCensusBoundary() { metadata.censusBoundaryOffset = 0; }

    // ZPage constructs its livemap before publishing the page in the page table
    // (zPage.cpp:42 _livemap(object_max_count())).
    void InitializeLiveMap();

    Generation GetOwnerGeneration() const;

    // ---- ZPage livemap surface (zPage.inline.hpp:57-70, 223-331, 371-392) ----
    // Names follow ZGC; the page type split (small/large) uses UnitRole until
    // ZPageType lands.
    int object_alignment_shift() const;
    uint32_t object_max_count() const;

    bool is_in(zaddress addr) const;

    bool is_marked() const;

    BitMap::idx_t bit_index(zaddress addr) const;
    MAddress offset_from_bit_index(BitMap::idx_t index) const;
    BaseObject* object_from_bit_index(BitMap::idx_t index) const;

    bool is_live_bit_set(zaddress addr) const;
    bool is_strong_bit_set(zaddress addr) const;

    bool is_object_live(zaddress addr) const;
    bool is_object_strongly_live(zaddress addr) const;
    bool is_object_marked_live(zaddress addr) const;
    bool is_object_marked_strong(zaddress addr) const;
    bool is_object_marked(zaddress addr, bool finalizable) const;

    // ZPage::mark_object: the only mark entry. Returns true when this call set
    // the bit(s); inc_live reports the first live claim for the caller's
    // ZMarkCache / inc_live accounting (zMark.cpp:405-425).
    bool mark_object(zaddress addr, bool finalizable, bool& inc_live);

    void inc_live(uint32_t objects, size_t bytes);
    uint32_t live_objects() const;
    size_t live_bytes() const;

    template <typename Function>
    void object_iterate(Function function);

    // zPage.inline.hpp:371-392: nearest object-start pair (strong or
    // finalizable) at or before a field address; 0 when no bit is found
    // (zaddress_unsafe::null).
    MAddress find_base_unsafe(MAddress p);
    MAddress find_base(MAddress p);

    void verify_live(uint32_t live_objects, size_t live_bytes, bool in_place) const;

    // ZPage::reset_livemap (zPage.cpp:115-117).
    void reset_livemap();

    ALWAYS_INLINE size_t GetAddressOffset(MAddress address) const;

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
        uintptr_t start;
        size_t size;
        size_t firstIndex;
        uintptr_t End() const { return start + size; }
    };

    static std::vector<UnitSegment> unitSegments;
    static ZGranuleMap<RegionInfo*> pageOwners;

    // zPageAllocator.hpp:166 ZSafeDelete<ZPage> _safe_destroy. The ABI keeps
    // page descriptors in the unit array (I6, PLAN §5), so the object whose
    // delete is deferred is a retirement record: its destructor reinitializes
    // the descriptor and hands the memory back. P03's independent ZPage
    // descriptor turns this into ZSafeDelete<ZPage> on the page allocator,
    // next to the page table this static sits beside today.
    struct PageRetirement {
        std::function<void()> retire;
        ~PageRetirement() { retire(); }
    };
    static ZSafeDelete<PageRetirement> safeDestroy;

    // zPageAllocator.cpp:2287-2293
    static void EnableSafeDestroy();
    static void DisableSafeDestroy();

    // zPageTable.cpp:83-98 ZGenerationPagesIterator: enable_safe_destroy in
    // the constructor, disable_safe_destroy in the destructor.
    class SafeDestroyScope {
    public:
        SafeDestroyScope() { EnableSafeDestroy(); }
        ~SafeDestroyScope() { DisableSafeDestroy(); }

        SafeDestroyScope(const SafeDestroyScope&) = delete;
        SafeDestroyScope& operator=(const SafeDestroyScope&) = delete;
    };

    static void RetirePage(RegionInfo* region, std::function<void()> retire);

    static size_t IndexedUnitCount(const std::vector<ZVirtualMemory>& ranges);
    static size_t IndexedUnitCount(const std::vector<UnitSegment>& segments);

    // Metadata over one unit range at an arbitrary native address (fixtures
    // that build a heap outside the zoffset address domain).
    static void Initialize(size_t nUnit, uintptr_t heapAddress)
    {
        InitializeSegments(heapAddress, { UnitSegment{ heapAddress, nUnit * UNIT_SIZE, 0 } });
    }

    // Metadata over the reserved heap address ranges (zoffset domain).
    static void InitializeSegments(uintptr_t metadataEnd, const std::vector<ZVirtualMemory>& ranges);
    static void InitializeSegments(uintptr_t metadataEnd, const std::vector<UnitSegment>& segments);

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

    static RegionInfo* InitRegion(size_t unitIdx, size_t nUnit, RegionInfo::UnitRole uclass,
                                  PageAge age = PageAge::old);

    static RegionInfo* InitRegionAt(uintptr_t addr, size_t nUnit, RegionInfo::UnitRole uclass);

    static MAddress GetUnitAddress(size_t unitIdx) { return UnitInfo::GetUnitAddress(unitIdx); }

    static void WaitCopiedBeforePayloadWipe(RegionInfo* region, const char* site);

    static void ClearUnits(size_t idx, size_t cnt);

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

    // ZGC has no allocPtr-linear object walk (zPage.inline.hpp:319-331 iterates
    // the livemap). Kept for the relocation residual sweep until the relocate
    // package retires ClearRelocationResiduals.
    void VisitAllObjects(const std::function<void(BaseObject*)>&& func);


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
    void PublishFromPageMetadata();

    // Product publication edge shared by forwarding and from-page liveness.
    // Keep this in the ordinary product inline path: the operation is part of
    // PrepareForwardableRegion, not a test-facing ABI surface.
    template<Generation G>
    __attribute__((always_inline)) inline void PublishForwardingCarrier();

    template<Generation G>
    void PrepareForwardableRegion();

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

    void SetYoungRegionFlag(uint8_t flag);

    bool IsYoungRegion() const;

    static size_t GetYoungRegionCount();

    static bool HasYoungRegions();

    // Promotion replaces current page metadata instead of retargeting the same
    // liveness object. The old Young livemap remains available only through the
    // from-page carrier (parked in retiredLivemap); the new Old current metadata
    // starts with a fresh livemap.
    void PromoteYoungRegion();

    // The original young ZPage left by ZPage::clone_for_promotion. The
    // region slot becomes old; this object owns the original, un-copied map.
    class PromotionPage {
    public:
        PromotionPage(std::unique_ptr<ZLiveMap> live, MAddress start, MAddress top, uint8_t age, bool large)
            : livemap(std::move(live)), start(start), top(top), age(age), large(large) {}
        void ObjectIterate(const std::function<void(BaseObject*)>& visitor) const;
        uint8_t Age() const { return age; }
    private:
        std::unique_ptr<ZLiveMap> livemap;
        MAddress start;
        MAddress top;
        uint8_t age;
        bool large;
    };

    std::unique_ptr<PromotionPage> CloneForPromotion();

    void SetYoungAge(uint8_t age);

    uint8_t GetYoungAge() const;

    RegionType GetRegionType() const;
    UnitRole GetUnitRole() const { return static_cast<UnitRole>(metadata.unitRole); }

    size_t GetUnitIdx() const { return RegionInfo::UnitInfo::GetUnitIdx(reinterpret_cast<const UnitInfo*>(this)); }

    MAddress GetRegionStart() const;

    MAddress GetRegionEnd() const;

    void SetRegionAllocPtr(MAddress addr) { metadata.allocPtr = addr; }

    MAddress GetRegionAllocPtr() const;







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
    // zRelocationSetSelector.cpp / zGeneration.cpp:216-221: a relocatable page
    // marked this cycle with no live bytes. Four names are kept for the
    // page-descriptor package to converge on is_relocatable && !is_marked.
    bool IsKnownEmpty() const;

    bool IsKnownYoungEmpty() const;

    bool IsSafeKnownEmpty();

    bool IsSafeKnownYoungEmpty();

    void RemoveFromList();

private:

    static std::atomic<size_t> youngRegionCount;
    static std::mutex youngRegionFlagMutex;
    static constexpr int32_t MAX_RAW_POINTER_COUNT = std::numeric_limits<int32_t>::max();
    static constexpr int32_t BIT_LENGTH = 4;
    static constexpr uint8_t YOUNG_AGE_BIT_LENGTH = 6;
    static constexpr uint8_t YOUNG_STATE_BIT_LENGTH = 1 + YOUNG_AGE_BIT_LENGTH;
    static constexpr uint8_t MAX_YOUNG_AGE = (1U << YOUNG_AGE_BIT_LENGTH) - 1;
    enum RegionStateBitPos : uint8_t {
        REGION_TYPE_FLAG = 0,
        TRACE_REGION_FLAG = BIT_LENGTH,
        IN_GHOST_FROM_REGION_FLAG,
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

        // ZPage::_livemap (zPage.hpp:52); see RegionInfo::livemap().
        ZLiveMap* livemap = nullptr;
        // The young livemap a promotion replaced. ZGC keeps the original ZPage
        // in the relocation set (zPage.cpp:64-72); the reused slot parks that
        // map here until the descriptor is retired, so a from-page reader
        // holding it through the forwarding carrier never sees it freed.
        ZLiveMap* retiredLivemap = nullptr;
        RegionInfo* ownerRegion = nullptr; // if unit is SUBORDINATE_UNIT

        RegionInfo* ownerRegion0 = nullptr; // if unit is SUBORDINATE_UNIT

        uint8_t regionLifeSequence = 0;
        // Borrow the immutable forwarding identity. Its owner reference is
        // released at the page lifecycle boundary, never reset in place.
        std::atomic<ZForwarding*> fwdOwner{ nullptr };
        // In-flight copiers holding an object lock.
        std::atomic<int32_t> copyInflight{ 0 };

        // resolveto: Compact packs densely; GetRoute prefix-sum dests are holes.
        // Table maps from-offset → actual dest for COMPACTED regions only.

        // ZPage::_seqnum / _seqnum_other (zPage.cpp:90-93).
        uint64_t birthSequence = 0;
        uint64_t otherSequence = 0;
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
            AtomicBitField<uint8_t> unitRoleBitField;
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
            };
            AtomicBitField<uint16_t> regionStateBitField;
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
        ZGenerationId _generation_id;
        RwLock rwLock;
    };

    class UnitInfo {
    public:
        // propgated from RegionManager
        static uintptr_t heapStartAddress; // exported ABI anchor: end of the reverse metadata array
        static size_t totalUnitCount;
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
                if (idx >= segment.firstIndex && idx - segment.firstIndex < segment.size / UNIT_SIZE) {
                    return segment.start + (idx - segment.firstIndex) * UNIT_SIZE;
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
    void InitRegionInfo(size_t nUnit, UnitRole uClass, PageAge age = PageAge::old);

    void InitRegion(size_t nUnit, UnitRole uClass, PageAge age = PageAge::old);

    static constexpr uint32_t NULLPTR_IDX = UnitInfo::INVALID_IDX;
    UnitMetadata metadata;
};
} // namespace MapleRuntime

#include "Heap/z/zPage.inline.hpp"
#endif // MRT_REGION_INFO_H
