// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZPAGE_H
#define MRT_ZPAGE_H

#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zPageType.hpp"
#include "Heap/z/zPageFwd.hpp"
#include "Heap/Allocator/RegionListTypes.hpp"

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
#include "Heap/z/zPageTable.hpp"

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

// Atomic accessor for C++ bit fields packed in ZPageRelocationScratch.
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

class ZPage {
    friend class ForwardingTable;
private:
    const ZPageType _type;
    ZGenerationId _generation_id;
    PageAge _age;
    uint32_t _seqnum;
    uint32_t _seqnum_other;
    uint32_t _partition_id;
    const ZVirtualMemory _virtual;
    volatile zoffset_end _top;
    ZLiveMap _livemap;
    bool _relocate_promoted;
public:
    using Page = ZPage;
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
    void reset_seqnum() { ResetPageSequence(); }
    uint64_t BirthSequence() const { return _seqnum; }
    uint64_t OtherSequence() const { return _seqnum_other; }
    uint32_t seqnum() const { return _seqnum; }
    bool IsAllocating() const;
    bool IsRelocatable() const;
    bool is_allocating() const { return IsAllocating(); }
    bool is_relocatable() const { return IsRelocatable(); }

    ZPageType type() const { return _type; }
    bool is_small() const { return _type == ZPageType::small; }
    bool is_medium() const { return _type == ZPageType::medium; }
    bool is_large() const { return _type == ZPageType::large; }
    const char* type_to_string() const;
    size_t object_alignment() const;
    zoffset start() const { return _virtual.start(); }
    zoffset_end end() const { return _virtual.end(); }
    size_t size() const { return _virtual.size(); }
    MAddress top() const { return GetRegionAllocPtr(); }
    size_t remaining() const
    {
        return GetRegionEnd() > GetRegionAllocPtr() ? GetRegionEnd() - GetRegionAllocPtr() : 0;
    }
    size_t used() const { return GetRegionAllocatedSize(); }
    ZPage* clone_for_promotion() const;
    uintptr_t alloc_object(size_t size);
    uintptr_t alloc_object_atomic(size_t size);
    bool undo_alloc_object(uintptr_t addr, size_t size);
    bool undo_alloc_object_atomic(uintptr_t addr, size_t size);
    ZPage* reset(PageAge age);

    ZPage(ZPageType type, PageAge age, const ZVirtualMemory& vmem);

    uint8_t GetRegionLifeSeq() const
    {
        return static_cast<uint8_t>(__atomic_load_n(&_scratch.regionLifeSequence, __ATOMIC_ACQUIRE));
    }

    RegionLifeId GetRegionLifeId() const
    {
        return _scratch.regionLifeId.load(std::memory_order_acquire);
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

    ZPage();
    static ZPage* NullRegion();

    ZLiveMap& livemap();
    const ZLiveMap& livemap() const;

    ZForwarding* GetFromPageCarrier() const;

    const ZForwarding::FromPageView* GetFromPageView() const
    {
        return ForwardingTable::GetFromPageView(const_cast<ZPage*>(this));
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
        return GetRegionStart() + _scratch.censusBoundaryOffset;
    }

    void StampCensusBoundary();
    void ResetCensusBoundary() { _scratch.censusBoundaryOffset = 0; }

    Generation GetOwnerGeneration() const;

    // ---- ZPage livemap surface (zPage.inline.hpp:57-70, 223-331, 371-392) ----
    // Names follow ZGC.
    static uint32_t object_max_count_for(ZPageType type, size_t size);
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

    static void RetirePage(ZPage* region, std::function<void()> retire);

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

    static void VisitPageOwners(const std::function<void(ZPage*)>& visitor);

    static ZPage* GetZPage(uint32_t idx);

    static bool InGhostFromRegion(BaseObject* obj)
    {
        return GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj)) != nullptr;
    }

    static ZPage* GetGhostFromRegionAt(uintptr_t allocAddr);

#if defined(MRT_GC_UNIT_TESTS)
    using GhostLookupTestHook = void (*)(ZPage*);
    MRT_EXPORT static void SetGhostLookupTestHook(GhostLookupTestHook hook);
    MRT_EXPORT static size_t GhostLookupTestHookCalls();


#endif

    static void InitFreeRegion(size_t unitIdx, size_t nUnit);

    static ZPage* InitRegion(size_t unitIdx, size_t nUnit, ZPageType uclass,
                                  PageAge age = PageAge::old);

    static ZPage* InitRegionAt(uintptr_t addr, size_t nUnit, ZPageType uclass);

    static void WaitCopiedBeforePayloadWipe(ZPage* region, const char* site);

    static void ClearUnits(size_t idx, size_t cnt);

    BaseObject* GetFirstObject() const { return from_region_addr(GetRegionStart()); }

    bool IsEmpty() const;

    size_t GetRegionSize() const;

    // Read-only, defensive extent for the phase-1 detach census. InitZPage
    // calls the census before _scratch.regionEnd is installed on a never-used
    // unit, so that case is one unit rather than an underflowed stale extent.
    size_t GetRegionSizeForDetachCheck() const;

    size_t GetUnitCount() const { return GetRegionSize() / UNIT_SIZE; }

    size_t GetGhostRegionSize() const;

    size_t GetGhostRegionUnitCount() const { return GetGhostRegionSize() / UNIT_SIZE; }

    size_t GetAvailableSize() const;

    size_t GetRegionAllocatedSize() const { return GetRegionAllocPtr() - GetRegionStart(); }

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    void DumpZPage(LogType type) const;
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
    static void RunGhostLookupTestHook(ZPage* region);
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
        explicit RetainScope(ZPage* region) : RetainScope(ForwardingTable::RetainPageOwner(region)) {}
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
        bool covers(ZPage* page) const { return retained && region == page; }
        ZForwarding* forwarding() const { return owner.get(); }
        ForwardingTable::Owner HoldForwarding() const { return owner; }

        RetainScope(const RetainScope&) = delete;
        RetainScope& operator=(const RetainScope&) = delete;
        RetainScope(RetainScope&&) = delete;
        RetainScope& operator=(RetainScope&&) = delete;

    private:
        ForwardingTable::Owner owner;
        ZPage* region;
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
        return _scratch.fwdOwner.load(std::memory_order_acquire);
    }

    int32_t CopyInflightWord() const
    {
        return _scratch.copyInflight.load(std::memory_order_acquire);
    }

    int32_t ForwardingRefCount() const;

    bool ForwardingClaimed() const;

    void LockWriteRegion() { _scratch.rwLock.LockWrite(); }

    void UnlockWriteRegion() { _scratch.rwLock.UnlockWrite(); }

    // zForwarding.cpp:110-181 in_place_relocation_claim_page + detach_page.
    class InPlaceClaimScope {
    public:
        MRT_EXPORT InPlaceClaimScope(ZPage* region, ZForwardingLife::Retire site);

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

    bool OnNamedList(const char* name) const;
    bool IsNotRelocatableThisCycle() const { return is_allocating(); }
    void SetInGhostRegion(uint8_t flag);

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
        PromotionPage(ZLiveMap* live, MAddress start, MAddress top, uint8_t age, bool large)
            : livemap(live), start(start), top(top), age(age), large(large) {}
        void ObjectIterate(const std::function<void(BaseObject*)>& visitor) const;
        uint8_t Age() const { return age; }
    private:
        ZLiveMap* livemap;
        MAddress start;
        MAddress top;
        uint8_t age;
        bool large;
    };

    std::unique_ptr<PromotionPage> CloneForPromotion();

    uint8_t GetYoungAge() const;




    size_t GetUnitIdx() const { return FindUnitIndex(GetRegionStart()); }

    MAddress GetRegionStart() const;

    MAddress GetRegionEnd() const;

    void SetRegionAllocPtr(MAddress addr) { _scratch.allocPtr = addr; }

    MAddress GetRegionAllocPtr() const;







    int32_t IncRawPointerObjectCount();

    int32_t DecRawPointerObjectCount();

    int32_t GetRawPointerObjectCount() const
    {
        return __atomic_load_n(&_scratch.rawPointerObjectCount, __ATOMIC_SEQ_CST);
    }

    bool CompareAndSwapRawPointerObjectCount(int32_t expectVal, int32_t newVal);

    uintptr_t Alloc(size_t size);

    // for regions shared by multithreads
    uintptr_t AtomicAlloc(size_t size);

    // zHeap.cpp:298-311 undo_alloc_object_for_relocation / zPage undo_alloc_object_atomic:
    // rewind allocPtr only if this object is still the bump tip. Failure is allowed.
    bool UndoAllocObjectAtomic(uintptr_t addr, size_t size);

    bool IsTraceRegion() const { return false; }

    // copyable during concurrent copying gc.
    bool IsSmallRegion() const;

    bool IsLargeRegion() const;

    bool IsThreadLocalRegion() const { return OnNamedList("thread local regions"); }

    bool IsPinnedRegion() const;

    ZPage* GetPrevRegion() const;

    // Intrusive-list authority. A region has at most one owning RegionList;
    // ghost snapshots intentionally do not modify this token.
    RegionList* GetRegionListOwner() const { return _scratch.regionListOwner.load(std::memory_order_acquire); }

    void SetRegionListOwner(RegionList* owner) { _scratch.regionListOwner.store(owner, std::memory_order_release); }

    void SetPrevRegion(const ZPage* r);

    ZPage* GetNextRegion() const;

    ZPage* GetNextGhostRegion() const;

    void SetNextRegion(const ZPage* r);

    bool IsFromRegion() const { return OnNamedList("from regions"); }
    bool IsLoneFromRegion() const { return GetRegionListOwner() == nullptr && is_relocatable(); }
    bool IsUnmovableFromRegion() const;

    bool IsToRegion() const { return false; }

    bool IsGarbageRegion() const { return OnNamedList("garbage regions"); }
    bool IsFreeRegion() const { return GetRegionListOwner() == nullptr && !is_relocatable(); }

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
        IN_GHOST_FROM_REGION_FLAG = 5
    };

    // P11/P05 scratch. ZGC has no analogue; not part of the ZPage ten-field set.
    struct ZPageRelocationScratch {
        struct {
            uintptr_t allocPtr;
            uintptr_t regionEnd;

            uint32_t nextRegionIdx;
            uint32_t prevRegionIdx;

            int32_t rawPointerObjectCount;
            uint32_t censusBoundaryOffset;
        };

        std::atomic<RegionList*> regionListOwner{ nullptr };
        std::atomic<RegionLifeId> regionLifeId{ 0 };
        ZLiveMap* retiredLivemap = nullptr;
        ZPage* ownerRegion = nullptr;
        ZPage* ownerRegion0 = nullptr;
        uint8_t regionLifeSequence = 0;
        std::atomic<ZForwarding*> fwdOwner{ nullptr };
        std::atomic<int32_t> copyInflight{ 0 };
        alignas(8) char routeInfoPad[24]{};
        uint32_t nextRegionIdx0;
        union {
            struct {
                uint8_t inGhostFromRegion : 1;
            };
            AtomicBitField<uint16_t> regionStateBitField;
        };
        std::atomic<uint64_t> routeStateSnapshot{ 0 };
        RegionLifeId ghostLifeId = 0;
        RwLock rwLock;
    };

public:
    static uintptr_t heapStartAddress;
    static size_t totalUnitCount;
    constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();

    ALWAYS_INLINE static size_t GetUnitIdxAt(uintptr_t allocAddr)
    {
        const size_t idx = FindUnitIndex(allocAddr);
        CHECK_DETAIL(idx != INVALID_IDX, "address is outside heap reservations: %#zx", allocAddr);
        return idx;
    }

    static MAddress GetUnitAddress(size_t idx);

    void BumpRegionLifeId();

    // Reinitialization consumes an already retired descriptor. The allocator
    // must remove the old page and finish safe retirement before reaching here.
    void InitZPage(size_t nUnit, ZPageType uClass, PageAge age = PageAge::old, bool live = true);

    void InitRegion(size_t nUnit, ZPageType uClass, PageAge age = PageAge::old);

    static constexpr uint32_t NULLPTR_IDX = INVALID_IDX;
    ZPageRelocationScratch _scratch;
};
} // namespace MapleRuntime

#include "Heap/z/zPage.inline.hpp"
#endif // MRT_ZPAGE_H
