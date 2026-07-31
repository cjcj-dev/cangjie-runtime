// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_INFO_H
#define MRT_REGION_INFO_H

#include <algorithm>
#include <limits>
#include <atomic>
#include <list>
#include <map>
#include <set>
#include <thread>
#include <vector>
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "Base/Globals.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Base/RwLock.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/GcInfos.h"
#include "Heap/Collector/LiveInfo.h"
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
#include "Heap/GCDebugConfig.h"
#endif
#include "securec.h"
#ifdef CANGJIE_ASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
namespace {
static std::atomic<size_t> retainedDispelNoGhostCount{ 0 };
static std::atomic<size_t> retainedDispelPartialGhostCount{ 0 };
static std::atomic<size_t> retainedSnapshotRestampCount{ 0 };
// One-sided drift sentinel: a teardown early-exit found no ghost overlay to clear while
// a route carrier was still installed — the exact ghost/route split the teardown
// invariant hunts, on the path the end-of-teardown assert never reaches. Counted and
// sampled loudly in release (no abort until a corpus proves the state illegal
// everywhere), aborted in debug builds.
static std::atomic<size_t> ghostRouteOneSidedCount{ 0 };
}

template<typename T>
class BitField {
public:
    // pos: the position where the bit locates. It starts from 0.
    // bitLen: the length that is to be read.
    T GetAtomicValue(size_t pos, size_t bitLen) const
    {
        T value = __atomic_load_n(&fieldVal, __ATOMIC_ACQUIRE);
        T bitMask = ((1 << bitLen) - 1) << pos;
        return value & bitMask;
    }
    void SetAtomicValue(size_t pos, size_t bitLen, T newValue)
    {
        do {
            T oldValue = fieldVal;
            T bitMask = ((1 << bitLen) - 1) << pos;
            T unchangedBitMask = ~bitMask;
            T newFieldValue = ((newValue << pos) & bitMask) | (oldValue & unchangedBitMask);
            if (__atomic_compare_exchange_n(&fieldVal, &oldValue, newFieldValue, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                return;
            }
        } while (true);
    }

private:
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
    enum class RetainedLiveInfoState : uint8_t {
        NEVER_EXAMINED,
        SNAPSHOT_VALID,
        SNAPSHOT_EMPTY,
    };

    enum RouteState : uint8_t {
        NORMAL = 0,
        FORWARDABLE,
        ROUTING,
        ROUTED,
        COMPACTED,
        FORWARDED,
    };

    static const size_t UNIT_SIZE; // same as system page size

    // regarding a object as a large object when the size is greater than 8 units.
    static const size_t LARGE_OBJECT_DEFAULT_THRESHOLD;

    // release a large object when the size is greater than 4096KB.
    static constexpr size_t LARGE_OBJECT_RELEASE_THRESHOLD = 4096 * KB;

    // youngAge occupies the top bits of the uint16 region-state bitfield
    // (positions 10..12 after YOUNG_REGION_FLAG at 9); 3 bits leave the
    // field's remaining capacity untouched. Public so the promotion-age knob
    // (StickyLog) can clamp against the representable maximum.
    static constexpr int32_t YOUNG_AGE_BITS = 3;
    static constexpr uint8_t MAX_YOUNG_AGE = (1u << YOUNG_AGE_BITS) - 1;

    bool CompareExchangeRouteState(RouteState expected, RouteState newWord)
    {
#if defined(__x86_64__)
        bool success = __atomic_compare_exchange_n(&(metadata.routeState), &expected, newWord, true, __ATOMIC_ACQ_REL,
                                                   __ATOMIC_ACQUIRE);
#else
        // due to "Spurious Failure" of compare_exchange_weak, compare_exchange_strong is chosen.
        bool success = __atomic_compare_exchange_n(&(metadata.routeState), &expected, newWord, false, __ATOMIC_SEQ_CST,
                                                   __ATOMIC_ACQUIRE);
#endif
        return success;
    }

    RouteState GetRouteState() const
    {
        RouteState state = __atomic_load_n(&(metadata.routeState), std::memory_order_acquire);
        return state;
    }

    void SetRouteState(RouteState state) { __atomic_store_n(&(metadata.routeState), state, std::memory_order_release); }

    // Region identity epoch: routes and reuse guards expire only when the address identity
    // or route carrier lifetime ends. ClearLiveInfo must not advance this domain.
    uint64_t GetIdentityEpoch() const
    {
        return __atomic_load_n(&metadata.identityEpoch, std::memory_order_acquire);
    }

    // Retained snapshots have a second validity domain. Every identity transition also
    // ends old snapshot validity, while ClearLiveInfo advances only this counter.
    uint64_t GetSnapshotEpoch() const
    {
        return __atomic_load_n(&metadata.snapshotEpoch, std::memory_order_acquire);
    }

    void BumpSnapshotEpoch()
    {
        __atomic_fetch_add(&metadata.snapshotEpoch, 1, std::memory_order_acq_rel);
    }

    // Publish snapshot expiry before identity expiry. Once this returns, all carriers from
    // the old identity are definitionally stale in both domains.
    void BumpIdentityEpoch()
    {
        BumpSnapshotEpoch();
        __atomic_fetch_add(&metadata.identityEpoch, 1, std::memory_order_acq_rel);
    }

#ifdef MRT_REGION_EPOCH_TEST
    // Post-domain-split: identity is the domain routes bind to (harness max-epoch probe).
    void SetEpochForTest(uint64_t epoch) { __atomic_store_n(&metadata.identityEpoch, epoch, __ATOMIC_RELEASE); }
#endif

    bool IsCompacted() { return GetRouteState() == RouteState::COMPACTED; }

    bool IsRoutingState() { return GetRouteState() == RouteState::ROUTING; }

    bool TryLockRouting(RouteState curState)
    {
        if (IsRoutingState()) {
            return false;
        }
        return CompareExchangeRouteState(curState, RouteState::ROUTING);
    }

    size_t GetPreLiveBytesInGhostRegion(MAddress address)
    {
        DCHECK(metadata.liveInfo0 != nullptr);
        size_t offset = GetAddressOffset(address);
        return metadata.liveInfo0->GetPreLiveBytes(offset, GetGhostRegionSize());
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

    LiveInfo* GetGhostLiveInfo() const { return metadata.liveInfo0; }

    LiveInfo* GetRetainedLiveInfo() const { return metadata.retainedLiveInfo; }

    RetainedLiveInfoState GetRetainedLiveInfoState() const { return metadata.retainedLiveInfoState; }

    uint64_t GetRetainedLiveInfoEpoch() const { return metadata.retainedLiveInfoEpoch; }

    MAddress GetRetainedLiveInfoCoveredUpTo() const { return metadata.retainedLiveInfoCoveredUpTo; }

    // INVARIANT — a retained census, once published as SNAPSHOT_VALID, asserts:
    // every live object below retainedLiveInfoCoveredUpTo has a mark in the
    // retained bitmap. Any code that materializes a live object below that
    // boundary afterwards (slot free-list revival, in-place geometry rewrites,
    // any future in-place reuse) must use one of the complete dispositions:
    // synchronously mark it while the active barrier closes its new edges
    // (ENUM/TRACE/CLEAR_SATB_BUFFER); mark it after tracing has completed but
    // before the census is published (PREFORWARD/FORWARD); or lower both the
    // published retained boundary and any pending census boundary to the region
    // start, making the whole region part of the unconditional remset scan.
    // Publishing a marked-only prefix while omitting a revived holder makes
    // RescanRememberedSet skip that holder in a later minor collection.
    void PreserveRetainedLiveInfo()
    {
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        // Exclusive allocation boundary: objects allocated after this point are implicitly live.
        metadata.retainedLiveInfoCoveredUpTo = GetRegionAllocPtr();
        if (IsLargeRegion()) {
            if (GetLiveByteCount() == 0) {
                metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_EMPTY;
                return;
            }
            size_t objectCount = 0;
            VisitAllObjects([&objectCount](BaseObject*) { ++objectCount; });
            CHECK(objectCount == 1);
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            return;
        }
        // E6 (ii) / T9: LiveInfo present → SNAPSHOT_VALID (remset filters survivors).
        if (metadata.retainedLiveInfo != nullptr) {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            return;
        }
        // No LiveInfo + zero live bytes: empty region → EMPTY; non-empty alloc
        // (e.g. to-space after Forward never marked) → NEVER_EXAMINED so remset
        // full-scans. ⛔ Do not treat missing bitmap as death (RUNTIME_MAP §4).
        CHECK(GetLiveByteCount() == 0);
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_EMPTY;
        } else {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        }
    }

    MAddress GetCensusBoundary() const
    {
        return GetRegionStart() + metadata.censusBoundaryOffset;
    }

    // Record "everything allocated so far predates the imminent cycle's
    // snapshot". Caller must hold the world stopped (pre-major rollover STW).
    void StampCensusBoundary()
    {
        uintptr_t offset = GetRegionAllocPtr() - GetRegionStart();
        metadata.censusBoundaryOffset =
            static_cast<uint32_t>(std::min<uintptr_t>(offset, std::numeric_limits<uint32_t>::max()));
    }

    void ResetCensusBoundary() { metadata.censusBoundaryOffset = 0; }

    // Publish the retained census with bitmap authority ending at `boundary`
    // (exclusive): below it the retained bitmap decides survivorship, at or
    // above it remset consumers treat objects as implicitly live (the existing
    // coveredUpTo protocol, WCollector::RescanRememberedSet). The sticky
    // promote path passes the cycle's census boundary here because the mark
    // bitmap only carries a verdict for objects that already existed when the
    // cycle began; everything born later — ordinary bump allocation, pinned
    // bump-over-the-boundary, and forwarded copies (to-space shares
    // post-rollover thread-local regions) — sits above the boundary. The
    // tracer, weak-reference and finalizer machinery are never consulted or
    // touched: this publishes consumer-side metadata only.
    void PreserveRetainedLiveInfoUpTo(MAddress boundary)
    {
        CHECK(boundary >= GetRegionStart() && boundary <= GetRegionAllocPtr());
        if (IsLargeRegion()) {
            // Single-object region: the consumer visits the object without
            // consulting coveredUpTo; keep the plain publication.
            PreserveRetainedLiveInfo();
            return;
        }
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        metadata.retainedLiveInfoCoveredUpTo = boundary;
        if (metadata.retainedLiveInfo == nullptr && boundary > GetRegionStart()) {
            // A censused prefix without a bitmap cannot prove death; fall back
            // to the conservative full-scan state rather than publish silence.
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            return;
        }
        metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
    }

    ALWAYS_INLINE void PreserveRetainedLiveInfo(MAddress coveredUpToOverride)
    {
        if (coveredUpToOverride == GetRegionStart() && GetRegionAllocPtr() != GetRegionStart()) {
            CHECK(GetLiveByteCount() == 0);
            metadata.retainedLiveInfo = GetLiveInfo();
            metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            metadata.retainedLiveInfoCoveredUpTo = coveredUpToOverride;
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            return;
        }
        CHECK(coveredUpToOverride == GetRegionAllocPtr());
        PreserveRetainedLiveInfo();
    }

    // VALID and EMPTY are both snapshot results and share the same epoch validity protocol.
    bool IsRetainedSnapshotValid() const
    {
        if (metadata.retainedLiveInfoState == RetainedLiveInfoState::NEVER_EXAMINED) {
            return false;
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
                __atomic_store_n(&metadata.liveInfo, allocatedLiveInfo, std::memory_order_release);
                DLOG(REGION, "region %p@%#zx liveinfo %p", this, GetRegionStart(), metadata.liveInfo);
                return allocatedLiveInfo;
            }
        } while (true);

        return nullptr;
    }

    RegionBitmap* GetMarkBitmap()
    {
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        RegionBitmap* bitmap = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
        if (reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return nullptr;
        }
        return bitmap;
    }

    RegionBitmap* GetOrAllocMarkBitmap()
    {
        LiveInfo* liveInfo = GetOrAllocLiveInfo();
        do {
            RegionBitmap* bitmap = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
            if (UNLIKELY(reinterpret_cast<uintptr_t>(bitmap) == LiveInfo::TEMPORARY_PTR)) {
                continue;
            }
            if (LIKELY(bitmap != nullptr)) {
                return bitmap;
            }
            RegionBitmap* newValue = reinterpret_cast<RegionBitmap*>(LiveInfo::TEMPORARY_PTR);
            if (__atomic_compare_exchange_n(&liveInfo->markBitmap, &bitmap, newValue, false, std::memory_order_seq_cst,
                                            std::memory_order_relaxed)) {
                RegionBitmap* allocated =
                    ForwardDataManager::GetForwardDataManager().AllocateRegionBitmap(GetRegionSize());
                __atomic_store_n(&liveInfo->markBitmap, allocated, std::memory_order_release);
                DLOG(REGION, "region %p@%#zx markbitmap %p", this, GetRegionStart(), metadata.liveInfo->markBitmap);
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

    void ResetMarkBit()
    {
        SetMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
    }

    bool MarkObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            if (metadata.isMarked != 1) {
                SetMarkedRegionFlag(1);
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        size_t regionSize = offset + GetRegionEnd() - reinterpret_cast<MAddress>(obj);
        bool marked = GetOrAllocMarkBitmap()->MarkBits(offset, objSize, regionSize);
        CHECK(IsMarkedObject(offset));
        return marked;
    }

    bool MarkObject(const BaseObject* obj, size_t objSize)
    {
        if (IsLargeRegion()) {
            if (metadata.isMarked != 1) {
                SetMarkedRegionFlag(1);
                return false;
            }
            return true;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        size_t regionSize = offset + GetRegionEnd() - reinterpret_cast<MAddress>(obj);
        bool marked = GetOrAllocMarkBitmap()->MarkBits(offset, objSize, regionSize);
        CHECK(IsMarkedObject(offset));
        return marked;
    }

    bool ResurrectObject(const BaseObject* obj, size_t offset)
    {
        if (IsLargeRegion()) {
            if (metadata.isResurrected != 1) {
                SetResurrectedRegionFlag(1);
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        size_t regionSize = offset + GetRegionEnd() - reinterpret_cast<MAddress>(obj);
        bool marked = GetOrAllocResurrectBitmap()->MarkBits(offset, objSize, regionSize);
        CHECK(IsResurrectedObject(offset));
        return marked;
    }

    bool EnqueueObject(const BaseObject* obj, size_t offset)
    {
        if (IsLargeRegion()) {
            if (metadata.isEnqueued != 1) {
                SetEnqueuedRegionFlag(1);
                return false;
            }
            return true;
        }
        U32 objSize = obj->GetSize();
        size_t regionSize = offset + GetRegionEnd() - reinterpret_cast<MAddress>(obj);
        CHECK(regionSize > 0);
        bool marked = GetOrAllocEnqueueBitmap()->MarkBits(offset, objSize, regionSize);
        CHECK(IsEnqueuedObject(offset));
        return marked;
    }

    bool IsResurrectedObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            return (metadata.isResurrected == 1);
        }
        RegionBitmap* resurrectBitmap = GetResurrectBitmap();
        if (resurrectBitmap == nullptr) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return resurrectBitmap->IsMarked(offset);
    }

    bool IsResurrectedObject(size_t offset)
    {
        if (IsLargeRegion()) {
            return (metadata.isResurrected == 1);
        }
        RegionBitmap* resurrectBitmap = GetResurrectBitmap();
        if (resurrectBitmap == nullptr) {
            return false;
        }
        return resurrectBitmap->IsMarked(offset);
    }

    bool IsMarkedObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            return (metadata.isMarked == 1);
        }
        RegionBitmap* markBitmap = GetMarkBitmap();
        if (markBitmap == nullptr) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return markBitmap->IsMarked(offset);
    }

    bool IsMarkedObject(size_t offset)
    {
        if (IsLargeRegion()) {
            return (metadata.isMarked == 1);
        }
        RegionBitmap* markBitmap = GetMarkBitmap();
        if (markBitmap == nullptr) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    bool IsSurvivedObject(size_t offset)
    {
        if (IsLargeRegion()) {
            return metadata.isMarked == 1 || metadata.isResurrected == 1;
        }

        RegionBitmap* markBitmap = GetMarkBitmap();
        if (markBitmap && markBitmap->IsMarked(offset)) {
            return true;
        }

        RegionBitmap* resurrectBitmap = GetResurrectBitmap();
        if (resurrectBitmap && resurrectBitmap->IsMarked(offset)) {
            return true;
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

    static void Initialize(size_t nUnit, uintptr_t heapAddress)
    {
        UnitInfo::totalUnitCount = nUnit;
        UnitInfo::heapStartAddress = heapAddress;
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
    static RegionInfo* TryGetRegionInfoAt(uintptr_t allocAddr)
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
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfoAt(reinterpret_cast<uintptr_t>(obj));
        return unit->GetMetadata().regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) != 0;
    }

    static RegionInfo* GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfoAt(allocAddr);
        if (unit->GetMetadata().regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) == 0) {
            return nullptr;
        }
        if (LoadUnitRole0(unit) == UnitRole::SUBORDINATE_UNIT) {
            return unit->GetMetadata().ownerRegion0;
        }
        return reinterpret_cast<RegionInfo*>(unit);
    }

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

    static void ClearUnits(size_t idx, size_t cnt)
    {
        uintptr_t unitAddress = RegionInfo::GetUnitAddress(idx);
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        DLOG(REGION, "clear dirty units[%zu+%zu, %zu) @[%#zx+%zu, %#zx)", idx, cnt, idx + cnt, unitAddress, size,
             RegionInfo::GetUnitAddress(idx + cnt));
#if defined(CANGJIE_GC_DEBUG_EQUIPMENT)
        if (GCDebugConfig::FillReclaimedMemory(unitAddress, size)) {
            return;
        }
#endif
        MapleRuntime::MemorySet(unitAddress, size, 0, size);
    }

    static void ReleaseUnits(size_t idx, size_t cnt)
    {
        void* unitAddress = reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx));
        size_t size = cnt * RegionInfo::UNIT_SIZE;
        DLOG(REGION, "release physical memory for units [%zu+%zu, %zu) @[%p+%zu, 0x%zx)", idx, cnt, idx + cnt,
             unitAddress, size, RegionInfo::GetUnitAddress(idx + cnt));
#if defined(_WIN64)
        CHECK_E(UNLIKELY(!VirtualFree(unitAddress, size, MEM_DECOMMIT)), "VirtualFree failed in ReturnPage, errno: %s",
                GetLastError());

#elif defined(__APPLE__)
        MemorySet(reinterpret_cast<uintptr_t>(unitAddress), size, 0, size);
        void* ret = mmap(unitAddress, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (ret == MAP_FAILED) {
            LOG(RTLOG_ERROR, "region mmmap fixed failed");
        } else if (ret != reinterpret_cast<void*>(unitAddress)) {
            LOG(RTLOG_ERROR, "mmap fixed at wrong addr %p->%p", unitAddress, ret);
        }
#else
        (void)madvise(unitAddress, size, MADV_DONTNEED);
#endif
#ifdef CANGJIE_ASAN_SUPPORT
        Sanitizer::OnHeapMadvise(unitAddress, size);
#endif
    }

    BaseObject* GetFirstObject() const { return reinterpret_cast<BaseObject*>(GetRegionStart()); }

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

    size_t GetUnitCount() const { return GetRegionSize() / UNIT_SIZE; }

    size_t GetGhostRegionSize() const
    {
        MAddress regionStart = GetRegionStart();
        DCHECK(metadata.regionEnd0 > GetRegionStart());
        return metadata.regionEnd0 - regionStart;
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

    // reset so that this region can be reused for allocation
    void InitFreeUnits()
    {
        // Phase: STW / region write-lock (ReclaimRegion, ReleaseRegion).
        // R2 validity-end: reclaim/free (E9 constructive).
        BumpIdentityEpoch();
        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; ++i) {
            array[i].ToFreeRegion();
        }
    }

    // Install route geometry in the identity domain. Installation is a semantic advance,
    // so it does not advance either epoch.
    // hidden: the readback check below made this too large to inline everywhere, and an
    // internal route installer has no business appearing on the export surface.
    __attribute__((visibility("hidden")))
    void SetRouteInfo(uintptr_t to1, uint64_t to1used = 0, uint32_t to2 = RouteInfo::INVALID_VALUE)
    {
        // Phase: GC routing path (RouteOrCompactRegionImpl under ROUTING state).
        metadata.routeInfo.SetRouteInfo(to1, to1used, to2, GetIdentityEpoch());
        // Consumers require expected == identity && expected == install, which only
        // holds if install == identity held from the moment of installation. The stamp
        // one line up makes that true by construction; read it back so a future stamp
        // source or a torn seqlock publish fails here, at the single install point,
        // instead of as a permanent fail-closed route miss at every consumer.
        RouteInfo installed = AcquireRouteInfo();
        CHECK_DETAIL(installed.IsInstalled() && installed.GetInstallEpoch() == GetIdentityEpoch(),
                     "route install did not bind the current identity epoch on region %p", this);
    }

    __attribute__((always_inline, visibility("hidden"))) RouteInfo AcquireRouteInfo() const
    {
        return metadata.routeInfo.AcquireRouteInfo();
    }

    __attribute__((always_inline, visibility("hidden"))) BaseObject* GetRoute(
        BaseObject* fromObj, RouteInfo& routeInfo)
    {
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        if (UNLIKELY(preLiveBytes >= routeInfo.GetToRegion1UsedBytes() &&
                     routeInfo.GetToRegion2Idx() == RouteInfo::INVALID_VALUE)) {
            size_t offset = GetAddressOffset(fromAddress);
            LiveInfo* ghostLiveInfo = metadata.liveInfo0;
            bool survived = ghostLiveInfo != nullptr && ghostLiveInfo->IsSurvivedObject(offset);
            size_t bitmapLiveBytes = ghostLiveInfo == nullptr ? 0 : ghostLiveInfo->GetBitmapLiveBytes();
            size_t recomputedLiveBytes = ghostLiveInfo == nullptr ? 0 : ghostLiveInfo->RecomputeBitmapLiveBytes();
            const char* producer = (bitmapLiveBytes != routeInfo.GetToRegion1UsedBytes() ||
                recomputedLiveBytes != bitmapLiveBytes) ? "bitmap-liveByteCount-snapshot-mismatch" :
                !survived ? "old-tagged-ref-or-root-to-non-survivor" : "cross-generation-or-route-plan-mismatch";
            CHECK_E(true,
                "GC route verifier: producer=%s fromRegion=%p unit=%zu state=%u fromObj=%p offset=%zu "
                "survived=%u preLiveBytes=%zu bitmapLiveBytes=%zu recomputedLiveBytes=%zu "
                "currentLiveByteCount=%zu toRegion1UsedBytes=%zu toRegion2Idx=%u ghostLiveInfo=%p",
                producer, this, GetUnitIdx(), static_cast<unsigned>(GetRouteState()), fromObj, offset,
                static_cast<unsigned>(survived), static_cast<size_t>(preLiveBytes), bitmapLiveBytes,
                recomputedLiveBytes, GetLiveByteCount(), routeInfo.GetToRegion1UsedBytes(),
                routeInfo.GetToRegion2Idx(), ghostLiveInfo);
        }
        MAddress toAddr = routeInfo.GetRoute(preLiveBytes);
        return reinterpret_cast<BaseObject*>(toAddr);
    }

    __attribute__((always_inline, visibility("hidden"))) uint64_t GetRouteInstallEpoch() const
    {
        return AcquireRouteInfo().GetInstallEpoch();
    }

    // True iff the region still carries the caller's identity and one self-consistent
    // acquired route record installed under that same identity.
    //
    // Both halves are required: the install stamp alone would accept a route that was
    // reinstalled after the caller's region view expired. This is the single definition
    // of route-carrier validity — RegionManager::RouteObject consumes it rather than
    // repeating the predicate inline. (A one-argument wrapper that acquired its own
    // route record existed for the review harness; it had no production caller and is
    // gone — a caller without a held RouteInfo has no business asking, because a
    // self-acquired record can only answer for the instant of the call.)
    bool RouteEpochMatches(uint64_t expectedEpoch, const RouteInfo& routeInfo) const
    {
        return expectedEpoch == GetIdentityEpoch() && routeInfo.IsInstalled() &&
            expectedEpoch == routeInfo.GetInstallEpoch();
    }

    void PrepareForwardableRegion()
    {
        CHECK(IsFromRegion());
        CHECK(static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS);
        CHECK(!IsGhostFromRegion());
        // Phase: STW (PrepareFromRegionList). Assemble is a semantic advance,
        // not identity expiry — no epoch bump in either domain.
        SetRouteState(FORWARDABLE);
        SetUnitRole0(static_cast<UnitRole>(metadata.unitRole));
        metadata.liveInfo0 = metadata.liveInfo;
        metadata.regionEnd0 = metadata.regionEnd;
        // Publish an empty carrier without consuming an epoch value.
        metadata.routeInfo.ClearRouteInfo();
        if (GetLiveByteCount() > 0) {
            SetInGhostRegion(1);
        }

        metadata.nextRegionIdx0 = metadata.nextRegionIdx;

        // prepare all units of this region.
        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 1; i < nUnit; i++) {
            UnitMetadata& mdata = array[i].GetMetadata();
            CHECK(static_cast<UnitRole>(mdata.unitRole) == UnitRole::SUBORDINATE_UNIT);
            CHECK(mdata.ownerRegion == this);
            CHECK(mdata.regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) == 0);

            array[i].SetUnitRole0(UnitRole::SUBORDINATE_UNIT);
            mdata.ownerRegion0 = this;
            if (GetLiveByteCount() > 0) {
                array[i].SetInGhostRegion(1);
            }
        }
    }

    void ClearGhostRegionBit()
    {
        if (IsGhostFromRegion()) {
            // POST_TRACE AddRawPointerObject path; no STW or region lock is held.
            // Clearing the unique route guard ends this carrier lifetime.
            uint64_t oldSnapshotEpoch = GetSnapshotEpoch();
            bool restampRetainedSnapshot =
                metadata.retainedLiveInfoState != RetainedLiveInfoState::NEVER_EXAMINED &&
                metadata.retainedLiveInfoEpoch == oldSnapshotEpoch;
            BumpIdentityEpoch();
            // Teardown publishes carrier absence (seqlock protocol); no epoch sentinel values.
            metadata.routeInfo.ClearRouteInfo();
            size_t nUnit = GetUnitCount();
            UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
            UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
            for (size_t i = 0; i < nUnit; i++) {
                array[i].SetInGhostRegion(0);
            }
            if (restampRetainedSnapshot) {
                metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            }
            AssertGhostRouteTornDown("ClearGhostRegionBit", nUnit);
        } else if (UNLIKELY(AcquireRouteInfo().IsInstalled())) {
            // Head bit already clear but a route carrier still installed: the same
            // one-sided drift the Dispel early exit watches for, observed here because
            // this caller may have just removed the region from the from-list — a
            // region that never reaches the ghost list never reaches Dispel's sentinel,
            // so deferral is not a guarantee on this path.
            size_t m = ghostRouteOneSidedCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((m & (m - 1)) == 0) {
                VLOG(REPORT,
                     "[ClearGhostRegionBit] ghost_route_one_sided region=%p identity_epoch=%llu n=%zu",
                     this, static_cast<unsigned long long>(GetIdentityEpoch()), m);
            }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            CHECK_DETAIL(false, "route carrier installed with no ghost overlay on region %p", this);
#endif
        }
    }

    // Ghost discoverability (the unit bits) and the route carrier (RouteInfo) answer the
    // same lifetime question through two structures; every teardown must end both, or a
    // reader that discovers the region through the surviving one consumes state the
    // other already declared dead. Checked at the end of both teardown paths, over the
    // exact extent the caller's own clearing loop walked (passed in, not re-read: the
    // Clear path holds no lock, and its extent is the current slice only — a partial
    // historical overlay from a smaller reuse is Dispel's to close, over the saved
    // extent). One extra O(units) pass per teardown. Unit bits are read through the
    // atomic bitfield accessor: teardown writers are monotone clearers, but this
    // checker runs on the no-lock path and must not invent its own weaker read.
    __attribute__((visibility("hidden")))
    void AssertGhostRouteTornDown(const char* who, size_t nUnit)
    {
        CHECK_DETAIL(!AcquireRouteInfo().IsInstalled(),
                     "%s left an installed route carrier on region %p", who, this);
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; i++) {
            CHECK_DETAIL(array[i].GetMetadata().regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) == 0,
                         "%s left ghost unit %zu discoverable on region %p", who, i, this);
        }
    }

    // dispel all units of this region.
    // inGhostFromRegion is the unique guard condition.

    void DispelGhostFromRegion()
    {
        // Phase: POST_TRACE PrepareForwardTable, after InvalidateOldTaggedRefsBeforeDispel's local STW; no region lock.
        // R2 validity-end: route teardown (not Forward complete).
        size_t nUnit = GetGhostRegionUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        size_t ghostUnitCount = 0;
        for (size_t i = 0; i < nUnit; ++i) {
            if (array[i].GetMetadata().regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) != 0) {
                ++ghostUnitCount;
            }
        }
        if (ghostUnitCount == 0) {
            // The historical overlay is already undiscoverable. Do not let its stale list
            // entry bump the epoch of a re-used current region identity.
            size_t n = retainedDispelNoGhostCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[DispelGhostFromRegion] no_ghost_skip region=%p identity_epoch=%llu units=%zu n=%zu",
                     this, static_cast<unsigned long long>(GetIdentityEpoch()), nUnit, n);
            }
            if (UNLIKELY(AcquireRouteInfo().IsInstalled())) {
                size_t m = ghostRouteOneSidedCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((m & (m - 1)) == 0) {
                    VLOG(REPORT,
                         "[DispelGhostFromRegion] ghost_route_one_sided region=%p identity_epoch=%llu n=%zu",
                         this, static_cast<unsigned long long>(GetIdentityEpoch()), m);
                }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                CHECK_DETAIL(false, "route carrier installed with no ghost overlay on region %p", this);
#endif
            }
            return;
        }
        if (ghostUnitCount != nUnit) {
            // ClearGhostRegionBit uses the current extent, so a re-used smaller region can
            // leave a partial historical overlay. Dispel the entire saved extent below.
            size_t n = retainedDispelPartialGhostCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[DispelGhostFromRegion] partial_ghost region=%p present=%zu units=%zu n=%zu",
                     this, ghostUnitCount, nUnit, n);
            }
        }
        uint64_t oldSnapshotEpoch = GetSnapshotEpoch();
        bool restampRetainedSnapshot =
            metadata.retainedLiveInfoState != RetainedLiveInfoState::NEVER_EXAMINED &&
            metadata.retainedLiveInfoEpoch == oldSnapshotEpoch;
        BumpIdentityEpoch();
        SetRouteState(NORMAL);
        // Teardown ends the carrier lifetime by publishing absence (seqlock protocol);
        // do not restamp cleared geometry as current, no epoch sentinel values.
        metadata.routeInfo.ClearRouteInfo();
        for (size_t i = 0; i < nUnit; i++) {
            array[i].SetInGhostRegion(0);
        }
        if (restampRetainedSnapshot) {
            // Route teardown ends RouteInfo validity, but it does not change the retained
            // bitmap or coveredUpTo truth. Re-endorse only the snapshot stamped immediately
            // before this bump; never make the invalidated route geometry valid again.
            metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            size_t n = retainedSnapshotRestampCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[DispelGhostFromRegion] retained_snapshot_restamp region=%p old_epoch=%llu "
                     "new_epoch=%llu state=%u n=%zu",
                     this, static_cast<unsigned long long>(oldSnapshotEpoch),
                     static_cast<unsigned long long>(GetSnapshotEpoch()),
                     static_cast<unsigned>(metadata.retainedLiveInfoState), n);
            }
        }
        AssertGhostRouteTornDown("DispelGhostFromRegion", nUnit);
    }

    bool IsGhostFromRegion() const
    {
        return metadata.regionStateBitField.GetAtomicValue(IN_GHOST_FROM_REGION_FLAG, 1) != 0;
    }

    // the interface can only be used to clear live info after gc.
    void CheckAndClearLiveInfo(LiveInfo* liveInfo)
    {
        // Garbage region may be reused by other thread. For the sake of safety, we don't clean it here.
        // We will clean it before the region is accessible.
        if (IsGarbageRegion()) {
            return;
        }
        // Check the value whether is expected, in order to avoid resetting a reused region.
        if (metadata.liveInfo == liveInfo) {
            metadata.liveInfo = nullptr;
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
    }
    void ClearLiveInfo()
    {
        // Phase: STW GC (Assemble*GarbageCandidates / ClearAllLiveInfo / young prepare).
        // R2 validity-end: ClearLiveInfo (snapshot semantics flip).
        BumpSnapshotEpoch();
        if (metadata.liveInfo != nullptr) {
            metadata.liveInfo = nullptr;
        }
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(0);
        }
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
    }

    // only from-region should be locked.
    bool TryLockReadFromRegion()
    {
        if (metadata.rwLock.TryLockRead()) {
            if (IsFromRegion() || IsLoneFromRegion()) {
                return true;
            } else {
                metadata.rwLock.UnlockRead();
            }
        }
        return false;
    }

    void UnlockReadFromRegion() { metadata.rwLock.UnlockRead(); }

    void LockWriteRegion() { metadata.rwLock.LockWrite(); }

    void UnlockWriteRegion() { metadata.rwLock.UnlockWrite(); }

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
    void SetInGhostRegion(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
    }

    void SetMarkedRegionFlag(uint8_t flag)
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

    void SetYoungRegionFlag(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_REGION_FLAG, 1, flag);
    }
    void SetYoungAge(uint8_t age)
    {
        CHECK(age <= MAX_YOUNG_AGE);
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::YOUNG_AGE_FLAG, YOUNG_AGE_BITS, age);
    }

    RegionType GetRegionType() const { return static_cast<RegionType>(metadata.regionType); }
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

    bool IsYoungRegion() const { return metadata.isYoungRegion == 1; }
    uint8_t GetYoungAge() const { return metadata.youngAge; }

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

    bool IsFromRegion() const { return static_cast<RegionType>(metadata.regionType) == RegionType::FROM_REGION; }
    bool IsLoneFromRegion() const
    {
        return static_cast<RegionType>(metadata.regionType) == RegionType::LONE_FROM_REGION;
    }
    bool IsUnmovableFromRegion() const
    {
        return static_cast<RegionType>(metadata.regionType) == RegionType::UNMOVABLE_FROM_REGION ||
            static_cast<RegionType>(metadata.regionType) == RegionType::RAW_POINTER_PINNED_REGION;
    }

    bool IsToRegion() const { return static_cast<RegionType>(metadata.regionType) == RegionType::TO_REGION; }

    bool IsGarbageRegion() const { return static_cast<RegionType>(metadata.regionType) == RegionType::GARBAGE_REGION; }
    bool IsFreeRegion() const { return static_cast<UnitRole>(metadata.unitRole) == UnitRole::FREE_UNITS; }

    bool IsValidRegion() const
    {
        return static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS ||
            static_cast<UnitRole>(metadata.unitRole) == UnitRole::LARGE_SIZED_UNITS;
    }

    uint64_t GetLiveByteCount() const
    {
        return __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
    }

    void ResetLiveByteCount()
    {
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
    }

    void AddLiveByteCount(uint64_t count)
    {
        (void)__atomic_fetch_add(&metadata.liveByteCount, count, __ATOMIC_ACQ_REL);
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
    static constexpr int32_t MAX_RAW_POINTER_COUNT = std::numeric_limits<int32_t>::max();
    static constexpr int32_t BIT_LENGTH = 4;
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
            // Sticky minor census boundary, as an offset from the region start
            // (fills the padding slot; small/pinned regions are <= 2048KB so a
            // u32 offset always fits — large regions never consult it).
            // Objects at addresses >= regionStart + censusBoundaryOffset were
            // born after the current cycle's allocation rollover, so the cycle's
            // mark bitmap holds no verdict about them: the promoted retained
            // census must treat them as implicitly live (coveredUpTo protocol)
            // instead of publishing bitmap authority over them. Stamped under
            // the pre-major rollover STW; reset to 0 (= region start) whenever a
            // region enters service or its geometry is rebuilt (compaction).
            uint32_t censusBoundaryOffset;
        };

        union {
            LiveInfo* liveInfo = nullptr;
            RegionInfo* ownerRegion; // if unit is SUBORDINATE_UNIT
        };

        union {
            LiveInfo* liveInfo0 = nullptr;
            RegionInfo* ownerRegion0; // if unit is SUBORDINATE_UNIT
        };

        LiveInfo* retainedLiveInfo = nullptr;
        RetainedLiveInfoState retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        // Region epoch and exclusive allocation boundary when the retained snapshot was built.
        uint64_t retainedLiveInfoEpoch = 0;
        MAddress retainedLiveInfoCoveredUpTo = 0;

        uintptr_t regionEnd0;
        RouteInfo routeInfo;
        // Independent validity domains; neither counter is reset on reuse.
        uint64_t identityEpoch = 0;
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
                uint8_t isYoungRegion : 1;
                // Region age in surviving minors; width must match
                // YOUNG_AGE_BITS (promotion-age sweep needs ages above 1 —
                // with a 1-bit field SetYoungAge(2) silently truncated to 0
                // and the region could never promote).
                uint8_t youngAge : YOUNG_AGE_BITS;
            };
            BitField<uint16_t> regionStateBitField;
        };
        RouteState routeState; // todo: put in RouteInfo
        RwLock rwLock;
    };

    class UnitInfo {
    public:
        // propgated from RegionManager
        static uintptr_t heapStartAddress; // the address of the first region space to allocate objects
        static size_t totalUnitCount;

        constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();
        static size_t GetUnitIdxAt(uintptr_t allocAddr)
        {
            if (heapStartAddress <= allocAddr && allocAddr < (heapStartAddress + totalUnitCount * UNIT_SIZE)) {
                return (allocAddr - heapStartAddress) / UNIT_SIZE;
            }

            std::abort();
        }

        static UnitInfo* GetUnitInfoAt(uintptr_t allocAddr) { return GetUnitInfo(GetUnitIdxAt(allocAddr)); }

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
        void SetInGhostRegion(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
        }

        void SetMarkedRegionFlag(uint8_t flag)
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

    // unitRole guards the ownerRegion/liveInfo union, and its writers publish it with an acq_rel
    // compare-exchange (UnitInfo::InitSubordinateUnit, InitRegionInfo below). Read it with
    // acquire so that the union read which follows in GetRegionInfo/GetRegionInfoAt/
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

    // unitRole is the discriminator of the ownerRegion/liveInfo union and of allocPtr/regionEnd:
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
        SetUnitRole(UnitRole::FREE_UNITS);
        // End any prior ghost-route carrier before this unit is published for free/reuse.
        // (k8 knife rebased onto k7 carrier protocol: absence publication, no sentinel;
        //  routeState normalized so all three teardown paths agree on carrier end state.)
        SetRouteState(RouteState::NORMAL);
        metadata.routeInfo.ClearRouteInfo();
        SetInGhostRegion(0);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        metadata.censusBoundaryOffset = 0;
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        metadata.liveInfo = nullptr;
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
        // Phase: allocator TakeRegion / InitFreeRegion (free-manager lock or STW reclaim).
        // R2 validity-end: region re-alloc reuse (all prior carriers expire).
        BumpIdentityEpoch();
        SetRegionType(RegionType::FREE_REGION);
        SetTraceRegionFlag(0);
        SetMarkedRegionFlag(0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
        SetYoungRegionFlag(1);
        SetYoungAge(0);
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
    }

    static constexpr uint32_t NULLPTR_IDX = UnitInfo::INVALID_IDX;
    UnitMetadata metadata;
};
} // namespace MapleRuntime
#endif // MRT_REGION_INFO_H
