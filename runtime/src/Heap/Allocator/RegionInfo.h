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
#include <thread>
#include <vector>
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif
#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Base/RwLock.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Collector/GcInfos.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Allocator/RouteTicket.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/NullRouteCaller.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/TagReuseProbe.h"
#include "Heap/Verify/MarkWhyProbe.h"
#include "Heap/Verify/EatArmDiag.h"
#include "Heap/Verify/RouteDom.h"
#include "Heap/Verify/SealCheck.h"
#include "securec.h"
#ifdef CANGJIE_ASAN_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
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

    // sealcheck: mark face frozen for geometry (M3). Set at RouteRegion ROUTING entry.
    bool IsMarkFaceSealed() const
    {
        return __atomic_load_n(&metadata.markFaceSealed, std::memory_order_acquire) != 0;
    }
    void SetMarkFaceSealed(bool v)
    {
        __atomic_store_n(&metadata.markFaceSealed, static_cast<uint8_t>(v ? 1 : 0), std::memory_order_release);
    }

    uint64_t GetSnapshotEpoch() const
    {
        return __atomic_load_n(&metadata.snapshotEpoch, std::memory_order_acquire);
    }

    void BumpSnapshotEpoch()
    {
        __atomic_fetch_add(&metadata.snapshotEpoch, 1, std::memory_order_acq_rel);
    }

    bool IsCompacted() { return GetRouteState() == RouteState::COMPACTED; }

    bool IsRoutingState() { return GetRouteState() == RouteState::ROUTING; }

    bool TryLockRouting(RouteState curState)
    {
        if (IsRoutingState()) {
            return false;
        }
        return CompareExchangeRouteState(curState, RouteState::ROUTING);
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

    // Probe-only: raw ghost liveInfo0 (no TEMPORARY filter; ghost never uses TEMPORARY).
    LiveInfo* GetLiveInfo0ForProbe() const { return metadata.liveInfo0; }

    // permhit, probe-only: the route's own to-side plan. RouteInfo records a bare start
    // address plus a used-bytes split (LiveInfo.h:246-248) and carries no epoch, so a
    // to-region that was reclaimed and re-taken keeps answering the same geometry. Only
    // the recorded plan, next to the region that lives at that address now, separates
    // "no path ever filled this tip" from "a tip was filled and the memory was reused".
    // Precedent: GetLiveInfo0ForProbe.
    RouteInfo GetRouteInfoForProbe() const { return metadata.routeInfo; }

    // installdomain: if PrepareForwardable snapshotted a null liveInfo, GetRoute always
    // rejects. After MarkObject created current liveInfo, bind it as ghost while still
    // FORWARDABLE so the paint is route-visible (pointer-share, same as PrepareForwardable).
    void BindLiveInfo0FromLiveIfNull()
    {
        if (metadata.liveInfo0 != nullptr) {
            return;
        }
        LiveInfo* live = GetLiveInfo();
        if (live == nullptr) {
            return;
        }
        metadata.liveInfo0 = live;
        if (metadata.regionEnd0 == 0 || metadata.regionEnd0 < metadata.regionEnd) {
            metadata.regionEnd0 = metadata.regionEnd;
        }
    }

    LiveInfo* GetRetainedLiveInfo() const { return metadata.retainedLiveInfo; }

    RetainedLiveInfoState GetRetainedLiveInfoState() const { return metadata.retainedLiveInfoState; }

    uint64_t GetRetainedLiveInfoEpoch() const { return metadata.retainedLiveInfoEpoch; }

    MAddress GetRetainedLiveInfoCoveredUpTo() const { return metadata.retainedLiveInfoCoveredUpTo; }

    void PreserveRetainedLiveInfo()
    {
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        metadata.retainedLiveInfoCoveredUpTo = GetRegionAllocPtr();
        if (IsLargeRegion()) {
            if (GetLiveByteCount() == 0) {
                metadata.retainedLiveInfoState = GetRegionAllocPtr() <= GetRegionStart()
                    ? RetainedLiveInfoState::SNAPSHOT_EMPTY
                    : RetainedLiveInfoState::NEVER_EXAMINED;
                return;
            }
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            return;
        }
        if (metadata.retainedLiveInfo != nullptr) {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            return;
        }
        CHECK(GetLiveByteCount() == 0);
        metadata.retainedLiveInfoState = GetRegionAllocPtr() <= GetRegionStart()
            ? RetainedLiveInfoState::SNAPSHOT_EMPTY
            : RetainedLiveInfoState::NEVER_EXAMINED;
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
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        metadata.retainedLiveInfoCoveredUpTo = boundary;
        if (metadata.retainedLiveInfo == nullptr && boundary > GetRegionStart()) {
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
                // markepoch: stamp face to current region snapshot (ZGC reset→release_store seqnum).
                allocatedLiveInfo->markEpoch = GetSnapshotEpoch();
                allocatedLiveInfo->markBitmap = nullptr;
                allocatedLiveInfo->resurrectBitmap = nullptr;
                allocatedLiveInfo->enqueueBitmap = nullptr;
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
                MarkWhyProbe::NoteMarkBitmapAlloc(this, allocated);
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

    // livesame / ZGC zMark.inline.hpp + zBitMap.inline.hpp:inc_live — count only on 0→1.
    // MarkBits returns true if already marked; false on first paint. AddLive only then.
    bool MarkObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            if (metadata.isMarked != 1) {
                SetMarkedRegionFlag(1);
                AddLiveByteCount(obj->GetSize());
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
        RegionBitmap* writeBm = GetOrAllocMarkBitmap();
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::MarkObject");
        bool already = writeBm->MarkBits(offset, objSize, regionSize);
        if (!already) {
            AddLiveByteCount(objSize);
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized0");
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized0");
        CHECK(IsMarkedObject(offset));
        return already;
    }

    bool MarkObject(const BaseObject* obj, size_t objSize)
    {
        if (IsLargeRegion()) {
            if (metadata.isMarked != 1) {
                SetMarkedRegionFlag(1);
                AddLiveByteCount(objSize);
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
        RegionBitmap* writeBm = GetOrAllocMarkBitmap();
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::MarkObject_sized");
        bool already = writeBm->MarkBits(offset, objSize, regionSize);
        if (!already) {
            AddLiveByteCount(objSize);
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized");
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized");
        CHECK(IsMarkedObject(offset));
        return already;
    }

    bool ResurrectObject(const BaseObject* obj, size_t offset)
    {
        if (IsLargeRegion()) {
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
        RegionBitmap* bitmap = GetOrAllocResurrectBitmap();
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::ResurrectObject");
        bool already = bitmap->MarkBits(offset, objSize, regionSize);
        if (!already) {
            AddLiveByteCount(objSize);
        }
        CHECK(bitmap->IsMarked(offset));
        return already;
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
        MAddress regionStart = GetRegionStart();
        MAddress regionEnd = GetRegionEnd();
        CheckObjectSize(obj, objSize, regionStart, regionEnd);
        size_t regionSize = regionEnd - regionStart;
        CHECK(regionSize > 0);
        RegionBitmap* bitmap = GetOrAllocEnqueueBitmap();
        // enqueue face is not the route geometry face; still report if mark-face sealed.
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::EnqueueObject");
        bool marked = bitmap->MarkBits(offset, objSize, regionSize);
        CHECK(bitmap->IsMarked(offset));
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

    // markepoch: count reads of a LiveInfo whose markEpoch != region snapshotEpoch.
    // Default product still returns false (same as "no bit"); MRT_GCV2_MARK_EPOCH_ASSERT=1 aborts.
    // Design: ops/design/MARK_EPOCH_DISCIPLINE.md §5 (ZGC zLiveMap.inline.hpp:41-43).
    // Hot path: epoch match is load+cmp only (no atomic). Stale path always counts.
    static std::atomic<size_t> markEpochStaleReadCount;
    static std::atomic<bool> markEpochAtexitInstalled;

    static bool MarkEpochAssertEnabled()
    {
        static const bool on = []() {
            const char* v = std::getenv("MRT_GCV2_MARK_EPOCH_ASSERT");
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        return on;
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
    bool NoteMarkEpochOnRead(LiveInfo* liveInfo)
    {
        if (liveInfo == nullptr) {
            return false;
        }
        const uint64_t face = liveInfo->markEpoch;
        const uint64_t now = GetSnapshotEpoch();
        if (face == now) {
            return true;
        }
        EnsureMarkEpochAtexit();
        size_t n = markEpochStaleReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (MarkEpochAssertEnabled()) {
            LOG(RTLOG_FATAL,
                "[GCV2][mark-epoch] stale LiveInfo read region=%p faceEpoch=%llu regionEpoch=%llu n=%zu "
                "env=MRT_GCV2_MARK_EPOCH_ASSERT=1",
                this, static_cast<unsigned long long>(face), static_cast<unsigned long long>(now), n);
        }
        if (n <= 8) {
            LOG(RTLOG_ERROR,
                "[GCV2][mark-epoch] stale_read region=%p faceEpoch=%llu regionEpoch=%llu n=%zu",
                this, static_cast<unsigned long long>(face), static_cast<unsigned long long>(now), n);
        }
        return false;
    }

    bool IsMarkedObject(const BaseObject* obj)
    {
        if (IsLargeRegion()) {
            return (metadata.isMarked == 1);
        }
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return false;
        }
        // markepoch §5: stale face ⇒ unmarked (ZGC is_marked false before bit test).
        if (!NoteMarkEpochOnRead(liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
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
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return false;
        }
        if (!NoteMarkEpochOnRead(liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    bool IsSurvivedObject(size_t offset)
    {
        if (IsLargeRegion()) {
            return metadata.isMarked == 1 || metadata.isResurrected == 1;
        }

        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr || !NoteMarkEpochOnRead(liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap = __atomic_load_n(&liveInfo->markBitmap, std::memory_order_acquire);
        if (markBitmap != nullptr && reinterpret_cast<MAddress>(markBitmap) != LiveInfo::TEMPORARY_PTR &&
            markBitmap->IsMarked(offset)) {
            return true;
        }
        RegionBitmap* resurrectBitmap = __atomic_load_n(&liveInfo->resurrectBitmap, std::memory_order_acquire);
        if (resurrectBitmap != nullptr && reinterpret_cast<MAddress>(resurrectBitmap) != LiveInfo::TEMPORARY_PTR &&
            resurrectBitmap->IsMarked(offset)) {
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
        return unit->GetMetadata().inGhostFromRegion != 0;
    }

    static RegionInfo* GetGhostFromRegionAt(uintptr_t allocAddr)
    {
        UnitInfo* unit = RegionInfo::UnitInfo::GetUnitInfoAt(allocAddr);
        if (unit->GetMetadata().regionStateBitField.GetAtomicValue(
                RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1) == 0) {
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
        // gcfwdfix: ring of zeroed ranges for WAS_LIVE_BEFORE_CLEAR (MRT_GCV2_TRACE_CLEAR=1).
        TraceClear::NoteRange(static_cast<MAddress>(unitAddress), size, "clear_units", nullptr, 0);
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
        size_t nUnit = GetUnitCount();
        UnitInfo* unit = reinterpret_cast<UnitInfo*>(this);
        UnitInfo::UnitInfoArray array = UnitInfo::UnitInfoArray(unit, nUnit);
        for (size_t i = 0; i < nUnit; ++i) {
            array[i].ToFreeRegion();
        }
    }

    void SetRouteInfo(uintptr_t to1, uint32_t to1used = 0, uint32_t to2 = RouteInfo::INVALID_VALUE)
    {
        metadata.routeInfo.SetRouteInfo(to1, to1used, to2);
    }

    // Sole mint of RouteTicket. Guard logic = former GetRoute(BaseObject*) domain check
    // (IsSurvivedObject on liveInfo0). Miss = empty OptionalRouteTicket; never silent derive.
    // Anchor: ops/design/ROUTE_DOMAIN.md §2; former guard RegionInfo.h GetRoute.
    ATTR_WARN_UNUSED OptionalRouteTicket AdmitForRoute(BaseObject* fromObj)
    {
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        size_t offset = GetAddressOffset(fromAddress);
        LiveInfo* ghostLiveInfo = metadata.liveInfo0;
        if (ghostLiveInfo == nullptr || !ghostLiveInfo->IsSurvivedObject(offset)) {
            // H1/H2 producer diag (routeorigin): size mismatch vs mark miss.
            // Gate: MRT_GCV2_NULLROUTE_DIAG=1 (default off). Positive control: off → zero lines.
            static const bool nullRouteDiag = []() {
                const char* v = std::getenv("MRT_GCV2_NULLROUTE_DIAG");
                return v != nullptr && v[0] == '1' && v[1] == '\0';
            }();
            // eatarm: only ROUTED/FORWARDABLE/ROUTING (same exclusive arm as IOR CHECK).
            if (EatArmDiag::Enabled()) {
                RouteState rsEat = GetRouteState();
                if (rsEat == RouteState::ROUTED || rsEat == RouteState::FORWARDABLE ||
                    rsEat == RouteState::ROUTING) {
                    EatArmDiag::NoteIorTarget(fromObj, EatArmDiag::GetFixHost(), offset);
                }
            }
            if (nullRouteDiag) {
                // Prefer ROUTED (exclusive CHECK arm). Skip FORWARDED flood that
                // exhausts the sample budget before the size=16 region-end hits.
                RouteState rs = GetRouteState();
                if (rs == RouteState::ROUTED || rs == RouteState::FORWARDABLE ||
                    rs == RouteState::ROUTING) {
                    static std::atomic<size_t> g_nullRouteDiagN{ 0 };
                    size_t n = g_nullRouteDiagN.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n <= 64) {
                        size_t ghostSz = (metadata.regionEnd0 > GetRegionStart())
                            ? static_cast<size_t>(metadata.regionEnd0 - GetRegionStart())
                            : 0;
                        size_t curSz = GetRegionSize();
                        size_t bitCover = 0;
                        size_t wordCnt = 0;
                        bool markNull = true;
                        bool resNull = true;
                        bool liveMarked = false;
                        bool live0Marked = false;
                        bool regionMarked = false;
                        size_t allocOff = 0;
                        if (metadata.allocPtr > GetRegionStart()) {
                            allocOff = static_cast<size_t>(metadata.allocPtr - GetRegionStart());
                        }
                        if (ghostLiveInfo != nullptr) {
                            RegionBitmap* mb = ghostLiveInfo->markBitmap;
                            RegionBitmap* rb = ghostLiveInfo->resurrectBitmap;
                            markNull = (mb == nullptr);
                            resNull = (rb == nullptr);
                            if (mb != nullptr) {
                                wordCnt = mb->wordCnt.load(std::memory_order_acquire);
                                bitCover = wordCnt * kMarkedBytesPerBit * kBitsPerWord;
                                live0Marked = mb->IsMarked(offset);
                            }
                            if (!live0Marked && rb != nullptr) {
                                live0Marked = rb->IsMarked(offset);
                            }
                        }
                        LiveInfo* curLive = GetLiveInfo();
                        if (curLive != nullptr) {
                            liveMarked = curLive->IsSurvivedObject(offset);
                        }
                        regionMarked = IsSurvivedObject(offset);
                        // marklate: per-region last-alloc phase (no TLS).
                        // blackmark: isTraceAtAlloc + clearTraceCnt for H3.
                        AllocPhaseDiag::Lookup ap =
                            AllocPhaseDiag::Find(fromObj, GetRegionStart());
                        unsigned curIsTrace = static_cast<unsigned>(IsTraceRegion());
                        // iorsource/alotior: slot provenance + host (only on sample path).
                        BaseObject* hostObj =
                            reinterpret_cast<BaseObject*>(NullRouteCaller::Host());
                        uintptr_t slotAddr = NullRouteCaller::Slot();
                        if (hostObj == nullptr && slotAddr != 0) {
                            RegionInfo* hostReg = TryGetRegionInfoAt(slotAddr);
                            if (hostReg != nullptr && !hostReg->IsFreeRegion() &&
                                !hostReg->IsGarbageRegion()) {
                                hostReg->VisitAllObjects(
                                    [&hostObj, slotAddr](BaseObject* holder) {
                                        if (hostObj != nullptr || holder == nullptr ||
                                            !holder->HasRefField()) {
                                            return;
                                        }
                                        holder->ForEachRefField(
                                            [holder, &hostObj, slotAddr](RefField<>& field) {
                                                if (reinterpret_cast<uintptr_t>(&field) ==
                                                    slotAddr) {
                                                    hostObj = holder;
                                                }
                                            });
                                    });
                            }
                        }
                        unsigned hostKnown = 0;
                        unsigned hostMarked = 0;
                        unsigned hostYoung = 0;
                        unsigned hostType = 0;
                        unsigned hostFree = 0;
                        unsigned hostGarbage = 0;
                        unsigned hostGhost = 0;
                        size_t fieldOff = 0;
                        if (hostObj != nullptr &&
                            Heap::IsHeapAddress(reinterpret_cast<MAddress>(hostObj))) {
                            hostKnown = 1;
                            if (slotAddr >= reinterpret_cast<uintptr_t>(hostObj)) {
                                fieldOff = slotAddr - reinterpret_cast<uintptr_t>(hostObj);
                            }
                            RegionInfo* hr =
                                TryGetRegionInfoAt(reinterpret_cast<MAddress>(hostObj));
                            if (hr != nullptr) {
                                hostYoung = static_cast<unsigned>(hr->IsYoungRegion());
                                hostType = static_cast<unsigned>(hr->GetRegionType());
                                hostFree = static_cast<unsigned>(hr->IsFreeRegion());
                                hostGarbage = static_cast<unsigned>(hr->IsGarbageRegion());
                                hostGhost = static_cast<unsigned>(hr->IsFromRegion());
                                hostMarked = static_cast<unsigned>(hr->IsMarkedObject(hostObj));
                            }
                        }
                        LOG(RTLOG_ERROR,
                            "[GCV2][nullroute-diag] n=%zu obj=%p offset=%zu ghostSz=%zu curSz=%zu "
                            "bitCover=%zu wordCnt=%zu markNull=%u resNull=%u live0Surv=%u "
                            "curLiveSurv=%u regionSurv=%u routeState=%u liveBytes=%zu young=%u "
                            "type=%u oob=%u allocOff=%zu nearEnd=%u "
                            "allocPhaseFound=%u isRegionLast=%u usedFrozen=%u usedNear=%u "
                            "allocMutPhase=%u(%s) allocHeapPhase=%u(%s) allocInMarkNew=%u "
                            "lastObj=%#zx curIsTrace=%u isTraceAtAlloc=%u clearTraceCnt=%u "
                            "everWasTrace=%u caller=%s edgeSrc=%s slot=%#zx host=%p "
                            "fieldOff=%zu hostKnown=%u hostMarked=%u hostYoung=%u hostType=%u "
                            "hostFree=%u hostGarbage=%u hostGhost=%u",
                            n, fromObj, offset, ghostSz, curSz, bitCover, wordCnt,
                            static_cast<unsigned>(markNull), static_cast<unsigned>(resNull),
                            static_cast<unsigned>(live0Marked), static_cast<unsigned>(liveMarked),
                            static_cast<unsigned>(regionMarked),
                            static_cast<unsigned>(rs), GetLiveByteCount(),
                            static_cast<unsigned>(IsYoungRegion()),
                            static_cast<unsigned>(GetRegionType()),
                            static_cast<unsigned>(offset >= bitCover && bitCover > 0),
                            allocOff,
                            static_cast<unsigned>(ghostSz > 0 && offset + 16 >= ghostSz),
                            static_cast<unsigned>(ap.found),
                            static_cast<unsigned>(ap.isRegionLast),
                            static_cast<unsigned>(ap.usedFrozen),
                            static_cast<unsigned>(ap.usedNear),
                            static_cast<unsigned>(ap.mutatorPhase),
                            AllocPhaseDiag::PhaseName(ap.mutatorPhase),
                            static_cast<unsigned>(ap.heapPhase),
                            AllocPhaseDiag::PhaseName(ap.heapPhase),
                            static_cast<unsigned>(ap.found &&
                                AllocPhaseDiag::IsMarkNewPhase(ap.mutatorPhase)),
                            static_cast<size_t>(ap.lastObj),
                            curIsTrace,
                            static_cast<unsigned>(ap.isTraceAtAlloc),
                            static_cast<unsigned>(ap.clearTraceCnt),
                            static_cast<unsigned>(ap.everWasTrace),
                            NullRouteCaller::Current(), NullRouteCaller::EdgeSrc(),
                            static_cast<size_t>(slotAddr), hostObj, fieldOff, hostKnown,
                            hostMarked, hostYoung, hostType, hostFree, hostGarbage, hostGhost);
                    }
                }
            }
            return OptionalRouteTicket();
        }
        return OptionalRouteTicket(fromObj);
    }

    // Geometric derive; domain is guaranteed by RouteTicket. No survivor re-check.
    // Anchor: LiveInfo.h:230-245; LiveInfo.cpp:15-24; ops/design/ROUTE_DOMAIN.md §2.
    BaseObject* GetRoute(RouteTicket t)
    {
        BaseObject* fromObj = t.From();
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        MAddress toAddr = metadata.routeInfo.GetRoute(preLiveBytes);
        // routedom: observe mark-domain at geometric GetRoute call site (default off).
        if (RouteDom::Enabled()) {
            RouteDom::NoteRoute(this, fromObj, preLiveBytes, static_cast<uintptr_t>(toAddr));
        }
        return from_region_addr(toAddr);
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
        return metadata.routeInfo.GetRoute(preLiveBytes);
    }

    ZGenerationId generation_id() const { return metadata._generation_id; }

    void PrepareForwardableRegion()
    {
        CHECK(IsFromRegion());
        CHECK(static_cast<UnitRole>(metadata.unitRole) == UnitRole::SMALL_SIZED_UNITS);
        CHECK(metadata.inGhostFromRegion == 0);
        // marklate: freeze last-alloc phase before ghost snapshot (survives reuse).
        AllocPhaseDiag::FreezeRegion(GetRegionStart());
        metadata.routeState = FORWARDABLE;
        // sealcheck: snapshot is not yet sealed; geometry freeze is at RouteRegion ROUTING.
        SetMarkFaceSealed(false);
        SetUnitRole0(static_cast<UnitRole>(metadata.unitRole));
        metadata.liveInfo0 = metadata.liveInfo;
        metadata.regionEnd0 = metadata.regionEnd;
        metadata.routeInfo.SetRouteInfo(0);
        metadata._generation_id = IsYoungRegion() ? ZGenerationId::young : ZGenerationId::old;
        // fysfixb / a2e7ee37: always install ghost, including liveBytes==0.
        // ForwardRegion only early-exits IsKnownEmpty (LIVE_AUTHORITY|0). Regions with
        // liveBytes==0 but no mark authority (neverExamined) still call RouteRegion;
        // without ghost that hits CHECK(IsGhostFromRegion). Ghost retention also
        // holds dead-from until PrepareFromRegionList dispel (plainedge).
        SetInGhostRegion(1);

        metadata.nextRegionIdx0 = metadata.nextRegionIdx;

        // prepare all units of this region.
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
            array[i].SetInGhostRegion(1);
        }
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
                array[i].SetInGhostRegion(0);
            }
        }
    }

    // dispel all units of this region.
    // inGhostFromRegion is the unique guard condition.

    // T-D guardian (MINOR_CONCURRENCY_0805 §八): parallel windows assert this is frozen.
    // Public for reffix parallel window assert + positive-control inject.
    static std::atomic<size_t> dispelGhostCount;

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
        dispelGhostCount.fetch_add(1, std::memory_order_relaxed);
        size_t nUnit = GetGhostRegionUnitCount();
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
            array[i].SetInGhostRegion(0);
        }
        SetRouteState(NORMAL);
        SetMarkFaceSealed(false);
    }

    bool IsGhostFromRegion() const { return metadata.inGhostFromRegion == 1; }

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
            metadata.liveInfo = nullptr;
            // Tracking phase ended: live counter is no longer a mark-period truth.
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
        if (metadata.liveInfo0 == liveInfo) {
            metadata.liveInfo0 = nullptr;
        }
        if (metadata.retainedLiveInfo == liveInfo) {
            metadata.retainedLiveInfo = nullptr;
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
        }
    }
    void ClearLiveInfo()
    {
        UnitRole unitRole = LoadUnitRole(reinterpret_cast<UnitInfo*>(this));
        if (unitRole == UnitRole::FREE_UNITS) {
            return;
        }
        CHECK_DETAIL(unitRole == UnitRole::SMALL_SIZED_UNITS || unitRole == UnitRole::LARGE_SIZED_UNITS,
                     "ClearLiveInfo must be called on a region head");
        BumpSnapshotEpoch();
        if (metadata.liveInfo != nullptr) {
            metadata.liveInfo = nullptr;
        }
        // Same carrier rule as liveInfo/retained: mark-cycle start drops ghost too.
        // PrepareForwardableRegion copies liveInfo→liveInfo0; without this, liveInfo0
        // can outlive ReleaseMemory(previous tag) (tagreuse T2).
        metadata.liveInfo0 = nullptr;
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(0);
        }
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
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
            __atomic_store_n(&metadata.liveInfo, static_cast<LiveInfo*>(nullptr), std::memory_order_release);
            // Tracking phase for this LiveInfo ended with its backing store about to vanish.
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
        if (inRange(metadata.liveInfo0)) {
            metadata.liveInfo0 = nullptr;
        }
        if (inRange(metadata.retainedLiveInfo)) {
            metadata.retainedLiveInfo = nullptr;
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
        }
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

    void SetYoungRegionFlag(uint8_t flag);

    bool IsYoungRegion() const
    {
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::YOUNG_REGION_FLAG, 1) != 0;
    }

    static size_t GetYoungRegionCount();

    static bool HasYoungRegions();

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

    // liveByteCount: bit63 = LIVE_AUTHORITY (mark-period established), bits0-62 = live bytes.
    // densify / fragmentation still use the byte count; reclaim-empty uses IsKnownEmpty()
    // which mirrors ZGC page->is_marked() (mark face epoch), not the byte counter alone.
    static constexpr uint64_t LIVE_AUTHORITY_BIT = 1ull << 63;
    static constexpr uint64_t LIVE_BYTES_MASK = LIVE_AUTHORITY_BIT - 1ull;

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

    // ZGC zGeneration.cpp:205-225 / zPage.inline.hpp:223-225:
    //   free iff !page->is_marked() where is_marked = livemap.seqnum == generation.seqnum.
    // Ours: mark-period authority required (minor must not reclaim non-young on bare zero),
    // then empty iff this region has no *current-cycle* mark face
    // (liveInfo null/TEMPORARY, or LiveInfo.markEpoch != snapshotEpoch, or large isMarked==0).
    // liveByteCount alone is NOT the reclaim predicate (ZGC live_bytes is relocation only).
    bool IsKnownEmpty() const
    {
        uint64_t raw = __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
        if ((raw & LIVE_AUTHORITY_BIT) == 0) {
            return false;
        }
        if (IsLargeRegion()) {
            return metadata.isMarked == 0;
        }
        LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
        if (liveInfo == nullptr || reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
            return true;
        }
        // markepoch: stale face ⇒ unmarked (ZGC is_marked false before bit test).
        if (liveInfo->markEpoch != GetSnapshotEpoch()) {
            return true;
        }
        // Current-cycle mark face present ⇒ ZGC is_marked true ⇒ not empty for reclaim.
        return false;
    }

    bool IsSafeKnownEmpty()
    {
        if (!IsKnownEmpty()) {
            return false;
        }
        if (GetRegionAllocPtr() <= GetRegionStart()) {
            return true;
        }
        // Examined: either large, or we had a mark face this cycle that is now stale/null
        // (authority already required by IsKnownEmpty). Residual bitmap pointer may remain.
        return GetMarkBitmap() != nullptr || GetResurrectBitmap() != nullptr || IsLargeRegion() ||
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
    void ResetLiveMapAfterForward()
    {
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(0);
        }
        BumpSnapshotEpoch();
    }

    void AddLiveByteCount(uint64_t count)
    {
        uint64_t prev = __atomic_fetch_add(&metadata.liveByteCount, count, __ATOMIC_ACQ_REL);
        if ((prev & LIVE_AUTHORITY_BIT) == 0) {
            (void)__atomic_fetch_or(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, __ATOMIC_ACQ_REL);
        }
    }

    // ZGC ZPage::verify_live — live_objects/bytes must match livemap. Always-on counter;
    // MRT_GCV2_LIVE_CROSSCHECK=1 aborts on mismatch.
    void VerifyLiveBooks(const char* where)
    {
        liveCrossCheckCount.fetch_add(1, std::memory_order_relaxed);
        if (!IsLiveCountAuthoritative()) {
            return;
        }
        const uint64_t liveBytes = GetLiveByteCount();
        const bool emptyByMark = IsKnownEmpty();
        // Homology: liveBytes==0 ⇔ empty-by-mark (and vice versa) under authority.
        const bool emptyByLive = (liveBytes == 0);
        if (emptyByMark == emptyByLive) {
            return;
        }
        size_t n = liveCrossMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
        static const bool abortOn = []() {
            const char* v = std::getenv("MRT_GCV2_LIVE_CROSSCHECK");
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        if (!liveCrossAtexitInstalled.exchange(true, std::memory_order_relaxed)) {
            std::atexit([]() {
                std::fprintf(stderr, "[GCV2][livesame][crosscheck] atexit checks=%zu mismatch=%zu\n",
                             liveCrossCheckCount.load(std::memory_order_relaxed),
                             liveCrossMismatchCount.load(std::memory_order_relaxed));
                std::fflush(stderr);
            });
        }
        if (n <= 32 || abortOn) {
            LOG(abortOn ? RTLOG_FATAL : RTLOG_ERROR,
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
    // Product geometry only — reachable from GetRoute(RouteTicket). External product
    // callers cannot reach preLiveBytes without a ticket (ROUTE_DOMAIN.md §2).
    size_t GetPreLiveBytesInGhostRegion(MAddress address)
    {
        DCHECK(metadata.liveInfo0 != nullptr);
        size_t offset = GetAddressOffset(address);
        return metadata.liveInfo0->GetPreLiveBytes(offset, GetGhostRegionSize());
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
        const char* fatal = std::getenv("MRT_GCV2_TIPINHEAP_FATAL");
        if (fatal != nullptr && fatal[0] == '1' && fatal[1] == '\0') {
            LOG(RTLOG_FATAL,
                "[GCV2][tipguard][TYPEINFO_IN_HEAP_FATAL] obj=%p tip=%p objSize=%zu hits=%zu",
                obj, tip, objSize, n);
            std::abort();
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
            "regionEnd=%#zx allocPtr=%#zx regionType=%u young=%u phase=%u bitCap=%zu bitIdx=%zu align=%zu",
            obj, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(), static_cast<unsigned>(GetRegionType()),
            static_cast<unsigned>(IsYoungRegion()), static_cast<unsigned>(phase), bitCapacity, bitIndex,
            kMarkedBytesPerBit);
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

        LiveInfo* liveInfo = nullptr;
        RegionInfo* ownerRegion = nullptr; // if unit is SUBORDINATE_UNIT

        LiveInfo* liveInfo0 = nullptr;
        RegionInfo* ownerRegion0 = nullptr; // if unit is SUBORDINATE_UNIT

        LiveInfo* retainedLiveInfo = nullptr;
        RetainedLiveInfoState retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        uint64_t retainedLiveInfoEpoch = 0;
        MAddress retainedLiveInfoCoveredUpTo = 0;

        uintptr_t regionEnd0;
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
        RouteState routeState; // todo: put in RouteInfo
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

        constexpr static uint32_t INVALID_IDX = std::numeric_limits<uint32_t>::max();
        static size_t GetUnitIdxAt(uintptr_t allocAddr)
        {
            if (heapStartAddress <= allocAddr && allocAddr < (heapStartAddress + totalUnitCount * UNIT_SIZE)) {
                return (allocAddr - heapStartAddress) / UNIT_SIZE;
            }

            // Named fatal before abort so OOB addresses leave a greppable trail
            // (was bare std::abort; o2fail R3 = 7/17 UNMAPPED SIGABRT with zero text).
            LOG(RTLOG_FATAL, "GetUnitIdxAt OOB addr=%#zx heap=[%#zx, %#zx)",
                allocAddr, heapStartAddress, heapStartAddress + totalUnitCount * UNIT_SIZE);
            return 0;
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
        SetUnitRole(UnitRole::FREE_UNITS);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        metadata.censusBoundaryOffset = 0;
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        metadata.liveInfo = nullptr;
        metadata.liveInfo0 = nullptr;
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
        BumpSnapshotEpoch();
        SetRegionType(RegionType::FREE_REGION);
        SetTraceRegionFlag(0);
        SetNotRelocatableThisCycle(0);
        // Ghost lives in unit metadata, not payload: ClearUnits cannot clear it.
        // TakeRegion reuses garbage without DispelGhostFromRegion (RegionInfo.h:667-698).
        SetInGhostRegion(0);
        SetMarkedRegionFlag(0);
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
