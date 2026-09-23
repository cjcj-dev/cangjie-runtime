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
#include "Heap/z/zRememberedSet.hpp"
namespace MapleRuntime {

// Page lifecycle role. ZGC keeps this identity in the page table plus the
// relocation set (zPageTable.hpp:57-77, zGeneration.cpp:205-221). The
// intrusive page lists are retired in favour of this field (#710).
enum class ZPageRole : uint8_t {
    None = 0, // free, or a from-page claimed off its list ("lone")
    RecentFull,
    FullTrace,
    LargeTrace,
    From,
    Garbage,
    OldLarge,
    RecentLarge,
};

inline const char* RegionRoleName(ZPageRole role)
{
    switch (role) {
        case ZPageRole::None: return "none";
        case ZPageRole::RecentFull: return "recent full regions";
        case ZPageRole::FullTrace: return "full trace regions";
        case ZPageRole::LargeTrace: return "large trace regions";
        case ZPageRole::From: return "from regions";
        case ZPageRole::Garbage: return "garbage regions";
        case ZPageRole::OldLarge: return "old large regions";
        case ZPageRole::RecentLarge: return "recent large regions";
    }
    return "unknown";
}

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
    ZRememberedSet _remembered_set;
    bool _relocate_promoted;
    // RetirePage hook: run by ~ZPage so safeDestroy can stay ZSafeDelete<ZPage>
    // (zPageAllocator.cpp:2248-2250 ZSafeDelete<ZPage> _safe_destroy).
    std::function<void()> _retireHook;
public:
    using Page = ZPage;
    // The table serializes publication/unbinding of this facade's owner.
    friend class ForwardingTable;
public:

    unsigned RelocateObserve() const;


    // regarding a object as a large object when the size is greater than 8 units.

    // release a large object when the size is greater than 4096KB.
    static constexpr size_t LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB;

    // ZPage::generation()->seqnum(), shared by all pages in that generation.
    ZGeneration* generation();
    const ZGeneration* generation() const;
    void ResetPageSequence();
    void reset_seqnum() { ResetPageSequence(); }
    uint64_t BirthSequence() const { return _seqnum; }
    uint64_t OtherSequence() const { return _seqnum_other; }
    uint32_t seqnum() const { return _seqnum; }
    bool IsAllocating() const;
    bool IsRelocatable() const;
    bool is_allocating() const { return IsAllocating(); }
    bool is_relocatable() const { return IsRelocatable(); }
    friend class ZPageTest;

    ZPageType type() const { return _type; }
    uint32_t partition_id() const { return _partition_id; }
    void reset_top_for_allocation() { _top = to_zoffset_end(start()); }
    PageAge age() const { return _age; }
    bool is_young() const { return IsYoungRegion(); }
    bool is_small() const { return _type == ZPageType::small; }
    bool is_medium() const { return _type == ZPageType::medium; }
    bool is_large() const { return _type == ZPageType::large; }
    const char* type_to_string() const;
    size_t object_alignment() const;
    zoffset start() const { return _virtual.start(); }
    zoffset_end end() const { return _virtual.end(); }
    size_t size() const { return _virtual.size(); }
    zoffset_end top() const
    {
        zoffset_end value;
        __atomic_load(&_top, &value, __ATOMIC_ACQUIRE);
        return value;
    }
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
    static std::atomic<uint64_t>& EnrolBeforeFlip();
    static std::atomic<uint64_t>& EnrolAfterFlip();

    ZPage();
    ~ZPage();
    static ZPage* NullRegion();

    ZLiveMap& livemap();
    const ZLiveMap& livemap() const;

    ZForwarding* GetFromPageCarrier() const;

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

    uintptr_t local_offset(MAddress addr) const { return GetAddressOffset(addr); }
    MAddress global_offset(uintptr_t l_offset) const { return GetRegionStart() + l_offset; }

    void remember(volatile zpointer* p);
    bool is_remembered(volatile zpointer* p);
    bool was_remembered(volatile zpointer* p);
    void remset_alloc();
    void clear_remset_bit_non_par_current(uintptr_t l_offset);
    void clear_remset_range_non_par_current(uintptr_t l_offset, size_t size);
    void swap_remset_bitmaps();
    ZBitMap::ReverseIterator remset_reverse_iterator_previous();
    ZRememberedSet::Iterator remset_iterator_limited_current(uintptr_t l_offset, size_t size);
    ZRememberedSet::Iterator remset_iterator_limited_previous(uintptr_t l_offset, size_t size);
    template<typename Function>
    void oops_do_remembered(Function function);
    template<typename Function>
    void oops_do_remembered_in_live(Function function);
    template<typename Function>
    void oops_do_current_remembered(Function function);
    bool is_remset_cleared_current() const;
    bool is_remset_cleared_previous() const;
    void verify_remset_cleared_current() const;
    void verify_remset_cleared_previous() const;
    void clear_remset_previous();
    void* remset_current();

    void verify_live(uint32_t live_objects, size_t live_bytes, bool in_place) const;

    // ZPage::reset_livemap (zPage.cpp:115-117).
    void reset_livemap();

    ALWAYS_INLINE size_t GetAddressOffset(MAddress address) const;



    // Reservation boundaries for the compiler heap-slot address-domain ABI.
    // Page and backing maps use global granule offsets, including holes.
    struct ReservedSegment {
        uintptr_t start;
        size_t size;
        uintptr_t End() const { return start + size; }
    };

    static std::vector<ReservedSegment> reservedSegments;

    // zPageAllocator.cpp:2248-2250 ZSafeDelete<ZPage> _safe_destroy: the
    // deferred-deleted object is the ZPage descriptor itself. The RetirePage
    // memory handback rides _retireHook, run from ~ZPage.
    static ZSafeDelete<ZPage> safeDestroy;

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
    static void RetireDescriptor(ZPage* page);


    // Metadata over one unit range at an arbitrary native address (fixtures
    // that build a heap outside the zoffset address domain).
    static void Initialize(size_t pageSize, uintptr_t heapAddress)
    {
        InitializeSegments(heapAddress, { ReservedSegment{ heapAddress, pageSize } });
    }

    // Metadata over the reserved heap address ranges (zoffset domain).
    static void InitializeSegments(uintptr_t metadataEnd, const std::vector<ZVirtualMemory>& ranges);
    static void InitializeSegments(uintptr_t metadataEnd, const std::vector<ReservedSegment>& segments);

    static size_t GranuleIndex(uintptr_t address);

    static bool ContainsReservedRange(uintptr_t start, size_t size);

    static void VisitPageOwners(const std::function<void(ZPage*)>& visitor);



    static ZPage* InitRegion(size_t granuleIndex, size_t pageSize, ZPageType uclass,
                                  PageAge age = PageAge::old);

    static void WaitCopiedBeforePayloadWipe(ZPage* region, const char* site);


    BaseObject* GetFirstObject() const { return from_region_addr(GetRegionStart()); }

    bool IsEmpty() const;

    size_t GetRegionSize() const;

    // Read-only, defensive extent for the phase-1 detach census. InitZPage
    // calls the census before _scratch.regionEnd is installed on a never-used
    // unit, so that case is one unit rather than an underflowed stale extent.
    size_t GetRegionSizeForDetachCheck() const;


    size_t GetGhostRegionSize() const;


    size_t GetAvailableSize() const;

    size_t GetRegionAllocatedSize() const { return GetRegionAllocPtr() - GetRegionStart(); }


    // reset so that this region can be reused for allocation
    void RetirePageMemory();



    bool IsCompactRouteDestination(MAddress address) const;

    ZGenerationId generation_id() const;


    // T-D guardian (MINOR_CONCURRENCY_0805 §八): parallel windows assert this is frozen.
    // Public for reffix parallel window assert + positive-control inject.
    static std::atomic<size_t> tdWindowCount;

    static size_t GetTdWindowCount()
    {
        return tdWindowCount.load(std::memory_order_relaxed);
    }


    // ZForwarding::retain_page (zForwarding.cpp:86-108). Three-state: 0 refuses,
    // <0 waits for done then refuses, >0 CAS +1.
    bool RetainForwarding();

    void ReleaseForwarding();

    // ZForwarding::retain_page: the three-state count is the gate, not the list
    // type. After relocation, CollectRegion moves the region to garbage
    // while the payload is still live; mutator relocate must still pin it.
    bool TryLockReadFromRegion() { return RetainForwarding(); }

    void UnlockReadFromRegion() { ReleaseForwarding(); }

    // RAII retain_page / release_page. ok() is false when the page is already
    // released or claimed — the late reader must not touch from-side state.
    class RetainScope {
    public:
        explicit RetainScope(ZPage* region) : RetainScope(forwarding_for_page(region)) {}
        explicit RetainScope(ZForwarding* forwarding)
            : owner(forwarding), region(owner ? owner->page() : nullptr),
              retained(owner && owner->retain_page(&generation_relocate_queue((owner->from_age() == PageAge::old ? Generation::Old : Generation::Young))))
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
        ZForwarding* forwarding() const { return owner; }
        ZForwarding* HoldForwarding() const { return owner; }

        RetainScope(const RetainScope&) = delete;
        RetainScope& operator=(const RetainScope&) = delete;
        RetainScope(RetainScope&&) = delete;
        RetainScope& operator=(RetainScope&&) = delete;

    private:
        ZForwarding* owner;
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
        return forwarding_for_page(const_cast<ZPage*>(this));
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
        MRT_EXPORT InPlaceClaimScope(ZPage* region, ZForwarding::Retire site);

        ~InPlaceClaimScope()
        {
            if (!retiring) return;
            owner->release_page();
            if (ZForwarding::CurrentPageWork() != owner) owner->mark_done();
        }

        InPlaceClaimScope(const InPlaceClaimScope&) = delete;
        InPlaceClaimScope& operator=(const InPlaceClaimScope&) = delete;
        InPlaceClaimScope(InPlaceClaimScope&&) = delete;
        InPlaceClaimScope& operator=(InPlaceClaimScope&&) = delete;

    private:
        ZForwarding* owner;
        bool retiring{ false };
    };

    // These interfaces are used to make sure the writing operations of value in C++ Bit Field will be atomic.

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

    uint8_t GetYoungAge() const;




    size_t granule_index() const { return GranuleIndex(GetRegionStart()); }

    MAddress GetRegionStart() const;

    MAddress GetRegionEnd() const;

    void SetRegionAllocPtr(MAddress addr) { _top = to_zoffset_end(addr - ZAddressHeapBase); }

    MAddress GetRegionAllocPtr() const;







    // for regions shared by multithreads

    // zHeap.cpp:298-311 undo_alloc_object_for_relocation / zPage undo_alloc_object_atomic:
    // rewind allocPtr only if this object is still the bump tip. Failure is allowed.

    bool IsTraceRegion() const { return false; }

    // copyable during concurrent copying gc.
    bool IsSmallRegion() const;

    bool IsLargeRegion() const;



    ZPageRole GetRegionRole() const { return _scratch.regionRole.load(std::memory_order_acquire); }

    void SetRegionRole(ZPageRole role) { _scratch.regionRole.store(role, std::memory_order_release); }

    bool CASRegionRole(ZPageRole& expect, ZPageRole target)
    {
        return _scratch.regionRole.compare_exchange_strong(expect, target, std::memory_order_acq_rel,
                                                           std::memory_order_acquire);
    }
    bool IsFromRegion() const { return GetRegionRole() == ZPageRole::From; }
    bool IsLoneFromRegion() const { return GetRegionRole() == ZPageRole::None && is_relocatable(); }

    bool IsToRegion() const { return false; }

    bool IsGarbageRegion() const { return GetRegionRole() == ZPageRole::Garbage; }
    bool IsFreeRegion() const { return GetRegionRole() == ZPageRole::None && !is_relocatable(); }

    bool IsValidRegion() const;
    // zRelocationSetSelector.cpp / zGeneration.cpp:216-221: a relocatable page
    // marked this cycle with no live bytes. Four names are kept for the
    // page-descriptor package to converge on is_relocatable && !is_marked.
    bool IsKnownEmpty() const;

    bool IsKnownYoungEmpty() const;

    bool IsSafeKnownEmpty();

    bool IsSafeKnownYoungEmpty();

private:

    static std::atomic<size_t> youngRegionCount;
    static std::mutex youngRegionFlagMutex;
    static constexpr int32_t BIT_LENGTH = 4;
    static constexpr uint8_t YOUNG_AGE_BIT_LENGTH = 6;
    static constexpr uint8_t YOUNG_STATE_BIT_LENGTH = 1 + YOUNG_AGE_BIT_LENGTH;
    static constexpr uint8_t MAX_YOUNG_AGE = (1U << YOUNG_AGE_BIT_LENGTH) - 1;
    enum RegionStateBitPos : uint8_t {
        UNUSED_REGION_STATE_BIT = 0
    };

    // P11/P05 scratch. ZGC has no analogue; not part of the ZPage ten-field set.
    struct ZPageRelocationScratch {
        struct {

            uint32_t censusBoundaryOffset;
        };

        std::atomic<ZPageRole> regionRole{ ZPageRole::None };
        std::atomic<RegionLifeId> regionLifeId{ 0 };
        ZLiveMap* retiredLivemap = nullptr;
        ZPage* ownerRegion = nullptr;
        ZPage* ownerRegion0 = nullptr;
        uint8_t regionLifeSequence = 0;
        std::atomic<ZForwarding*> fwdOwner{ nullptr };
        std::atomic<int32_t> copyInflight{ 0 };
        alignas(8) char routeInfoPad[24]{};
        union {
            uint8_t unusedRegionStatePad;
            AtomicBitField<uint16_t> regionStateBitField;
        };
        std::atomic<uint64_t> routeStateSnapshot{ 0 };
        RegionLifeId ghostLifeId = 0;
        RwLock rwLock;
    };

public:
    static uintptr_t heapStartAddress;
    constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();

    ALWAYS_INLINE static size_t GranuleIndexAt(uintptr_t allocAddr)
    {
        const size_t idx = GranuleIndex(allocAddr);
        CHECK_DETAIL(idx != INVALID_IDX, "address is outside heap reservations: %#zx", allocAddr);
        return idx;
    }

    static MAddress GranuleAddress(size_t idx);

    void BumpRegionLifeId();

    // Reinitialization consumes an already retired descriptor. The allocator
    // must remove the old page and finish safe retirement before reaching here.
    void InitZPage(size_t pageSize, ZPageType uClass, PageAge age = PageAge::old, bool live = true);

    void InitRegion(size_t pageSize, ZPageType uClass, PageAge age = PageAge::old);

    static constexpr uint32_t NULLPTR_IDX = INVALID_IDX;
    ZPageRelocationScratch _scratch;
};
} // namespace MapleRuntime

#include "Heap/z/zPage.inline.hpp"
#endif // MRT_ZPAGE_H
