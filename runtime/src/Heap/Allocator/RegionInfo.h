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
#include "Heap/Collector/GcInfos.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Collector/ManagedObjectGate.h"
#include "Heap/Allocator/RouteTicket.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/NullRouteCaller.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/HeldFreeDiag.h"
#include "Heap/Verify/RegionLifeDiag.h"
#include "Heap/Verify/TagReuseProbe.h"
#include "Heap/Verify/MarkWhyProbe.h"
#include "Heap/Verify/EatArmDiag.h"
#include "Heap/Verify/RouteDestHold.h"
#include "Heap/Verify/RouteDom.h"
#include "Heap/Verify/SealCheck.h"
#include "Heap/Verify/TlRawDiag.h"
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
    using CompactRouteTable = std::unordered_map<size_t, MAddress>;

    enum class RetainedLiveInfoState : uint8_t {
        NEVER_EXAMINED,
        SNAPSHOT_VALID,
        SNAPSHOT_EMPTY,
    };

    // holderlive (F2): the only object-level holder-liveness filter we have reads
    // GetRetainedLiveInfoState() at WCollector.cpp:3579 and measured NEVER_EXAMINED for
    // 100% of holders (never=2787/originFound=2787 per minor). NEVER_EXAMINED has three
    // distinct producers and the state word cannot tell them apart:
    //   - nobody ever called Preserve* on this region during its current life,
    //   - Preserve* ran but had no live info to keep (it writes NEVER_EXAMINED itself),
    //   - Preserve* ran and stored a snapshot, then a clear path wiped it.
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
        return __atomic_load_n(&metadata.snapshotEpoch, std::memory_order_acquire);
    }

    template<Generation G>
    uint64_t GetMarkSnapshotEpoch() const
    {
        if (G == Generation::Young) {
            return __atomic_load_n(&metadata.youngSnapshotEpoch, std::memory_order_acquire) &
                YOUNG_SNAPSHOT_EPOCH_MASK;
        }
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
        return MarkView<G>(this, GetMarkSnapshotEpoch<G>());
    }

    void BumpSnapshotEpoch()
    {
        __atomic_fetch_add(&metadata.snapshotEpoch, 1, std::memory_order_acq_rel);
    }

    template<Generation G>
    void BumpMarkSnapshotEpoch()
    {
        if (G == Generation::Young) {
            __atomic_fetch_add(&metadata.youngSnapshotEpoch, 1, std::memory_order_acq_rel);
            return;
        }
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
            BumpMarkSnapshotEpoch<Generation::Young>();
            return;
        }
        size_t n = oneseqBumpInitRegion.fetch_add(1, std::memory_order_relaxed) + 1;
        BumpSnapshotEpoch();
        BumpMarkSnapshotEpoch<Generation::Young>();
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

    Generation GetRouteMarkGeneration() const
    {
        return (__atomic_load_n(&metadata.markFaceSealed, std::memory_order_acquire) & ROUTE_MARK_YOUNG_BIT) != 0
            ? Generation::Young : Generation::Old;
    }

    void SetRouteMarkGeneration(Generation generation)
    {
        if (generation == Generation::Young) {
            __atomic_fetch_or(&metadata.markFaceSealed, ROUTE_MARK_YOUNG_BIT, __ATOMIC_RELEASE);
        } else {
            __atomic_fetch_and(&metadata.markFaceSealed, static_cast<uint8_t>(~ROUTE_MARK_YOUNG_BIT),
                               __ATOMIC_RELEASE);
        }
    }

    template<Generation G>
    MarkView<G> GetRouteMarkView()
    {
        CHECK_DETAIL(GetRouteMarkGeneration() == G,
                     "route mark generation mismatch region=%p have=%u want=%u", this,
                     static_cast<unsigned>(GetRouteMarkGeneration()), static_cast<unsigned>(G));
        LiveInfo* face = metadata.liveInfo0;
        uint64_t epoch = face == nullptr ? GetMarkSnapshotEpoch<G>()
                                         : face->GetMarkFace<G>().epoch.load(std::memory_order_acquire);
        return MarkView<G>(this, epoch);
    }

    template<Generation G>
    uint64_t GetMarkEpoch(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        return liveInfo == nullptr ? 0 : liveInfo->GetMarkFace<G>().epoch.load(std::memory_order_acquire);
    }

    template<Generation G>
    RegionBitmap* GetMarkBitmap(MarkView<G> view, LiveInfo* liveInfo) const
    {
        CHECK(view.GetRegion() == this);
        if (liveInfo == nullptr ||
            liveInfo->GetMarkFace<G>().epoch.load(std::memory_order_acquire) != view.GetEpoch()) {
            return nullptr;
        }
        RegionBitmap* bitmap =
            __atomic_load_n(&liveInfo->GetMarkFace<G>().bitmap, std::memory_order_acquire);
        return reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR ? nullptr : bitmap;
    }

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, LiveInfo* liveInfo, size_t offset) const
    {
        CHECK(view.GetRegion() == this);
        return liveInfo != nullptr && liveInfo->IsSurvivedObject(view, offset);
    }

    bool IsRouteSurvivedObject(size_t offset)
    {
        LiveInfo* face = metadata.liveInfo0 != nullptr ? metadata.liveInfo0 : GetLiveInfo();
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            return IsSurvivedObject(view, face, offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        return IsSurvivedObject(view, face, offset);
    }

    bool IsRouteMarkedObject(size_t offset)
    {
        LiveInfo* face = metadata.liveInfo0 != nullptr ? metadata.liveInfo0 : GetLiveInfo();
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            if (IsLargeRegion()) {
                return GetMarkedRegionFlag(view) == 1;
            }
            RegionBitmap* bitmap = GetMarkBitmap(view, face);
            return bitmap != nullptr && bitmap->IsMarked(offset);
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1;
        }
        RegionBitmap* bitmap = GetMarkBitmap(view, face);
        return bitmap != nullptr && bitmap->IsMarked(offset);
    }

    bool IsRouteMarkedObject(const BaseObject* object)
    {
        return IsRouteMarkedObject(GetAddressOffset(reinterpret_cast<MAddress>(object)));
    }

    bool IsRouteKnownEmpty()
    {
        if (GetRouteMarkGeneration() == Generation::Young) {
            return IsKnownYoungEmpty(GetRouteMarkView<Generation::Young>());
        }
        return IsKnownEmpty(GetRouteMarkView<Generation::Old>());
    }

    RegionBitmap* GetRouteMarkBitmap(LiveInfo* face = nullptr)
    {
        LiveInfo* selected = face != nullptr ? face : GetLiveInfo();
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
        return GetRouteMarkGeneration() == Generation::Young
            ? GetMarkSnapshotEpoch<Generation::Young>()
            : GetMarkSnapshotEpoch<Generation::Old>();
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

    // holderlive (F2): the retained snapshot has to answer "was this holder live at the last
    // mark" during every minor until the next major re-marks the region. It cannot do that as a
    // borrowed LiveInfo*: LiveInfo lives in a per-tag arena that is recycled one GC cycle later
    // (ForwardDataManager::ClearPreviousForwardData → ReleaseMemory), and UnbindPreviousLiveInfo
    // (DoGarbageCollection, WCollector.cpp:6122 at 7924d28f) drops every borrowed pointer
    // into it at the end of each major.
    // Measured: 100% of remset holders read NEVER_EXAMINED, and for 2113/2115 of them the last
    // thing that touched the snapshot was that unbind ([RETLIVE][why-never] lastOp=clrChecked).
    // So keep our own copy of the bits — regionSize/512 bytes, allocated only for regions that
    // are actually preserved. Default off (MRT_GCV2_RETAINED_OWN_COPY=1).
    static bool RetainedOwnCopyEnabled()
    {
        static const bool enabled = []() {
            const char* value = std::getenv("MRT_GCV2_RETAINED_OWN_COPY");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }

    // Union of markBitmap and resurrectBitmap — the same two bitmaps LiveInfo::IsSurvivedObject
    // reads, collapsed into one array so the copy answers exactly the same question.
    void CaptureRetainedMarkWords(MarkView<Generation::Old> view, LiveInfo* liveInfo)
    {
        FreeRetainedMarkWords();
        if (liveInfo == nullptr) {
            return;
        }
        LiveInfo::MarkFace& oldFace = liveInfo->GetMarkFace<Generation::Old>();
        RegionBitmap* mark = oldFace.epoch.load(std::memory_order_acquire) == view.GetEpoch()
            ? __atomic_load_n(&oldFace.bitmap, std::memory_order_acquire) : nullptr;
        RegionBitmap* resurrect = liveInfo->resurrectBitmap;
        size_t markWords = mark == nullptr ? 0 : mark->wordCnt.load(std::memory_order_acquire);
        size_t resurrectWords = resurrect == nullptr ? 0 : resurrect->wordCnt.load(std::memory_order_acquire);
        size_t wordCnt = std::max(markWords, resurrectWords);
        if (wordCnt == 0) {
            return;
        }
        uint64_t* words = static_cast<uint64_t*>(malloc(wordCnt * sizeof(uint64_t)));
        if (words == nullptr) {
            // Out of memory for a diagnostic-grade copy: leave the snapshot absent. The
            // consumer treats "no snapshot" as keep, i.e. this degrades to today's fail-open.
            return;
        }
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

    bool HasRetainedMarkWords() const { return metadata.retainedMarkWords != nullptr; }

    // Same indexing as RegionBitmap::IsMarked.
    bool RetainedMarkWordsSay(size_t offset) const
    {
        if (metadata.retainedMarkWords == nullptr) {
            return false;
        }
        size_t bitIdx = offset / kMarkedBytesPerBit;
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

    uint32_t GetRetainedPreserveCount() const { return metadata.retainedPreserveCnt; }

    uint32_t GetRetainedClearCount() const { return metadata.retainedClearCnt; }

    uint8_t GetRetainedLastOp() const { return metadata.retainedLastOp; }

    void PreserveRetainedLiveInfo()
    {
        metadata.retainedLiveInfo = GetLiveInfo();
        metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
        metadata.retainedLiveInfoCoveredUpTo = GetRegionAllocPtr();
        if (RetainedOwnCopyEnabled() && !IsLargeRegion()) {
            CaptureRetainedMarkWords(GetMarkView<Generation::Old>(), metadata.retainedLiveInfo);
        }
        if (IsLargeRegion()) {
            if (GetLiveByteCount() == 0) {
                metadata.retainedLiveInfoState = GetRegionAllocPtr() <= GetRegionStart()
                    ? RetainedLiveInfoState::SNAPSHOT_EMPTY
                    : RetainedLiveInfoState::NEVER_EXAMINED;
                NoteRetainedPreserve();
                return;
            }
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            NoteRetainedPreserve();
            return;
        }
        if (metadata.retainedLiveInfo != nullptr) {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            NoteRetainedPreserve();
            return;
        }
        CHECK(GetLiveByteCount() == 0);
        metadata.retainedLiveInfoState = GetRegionAllocPtr() <= GetRegionStart()
            ? RetainedLiveInfoState::SNAPSHOT_EMPTY
            : RetainedLiveInfoState::NEVER_EXAMINED;
        NoteRetainedPreserve();
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
        if (RetainedOwnCopyEnabled()) {
            CaptureRetainedMarkWords(GetMarkView<Generation::Old>(), metadata.retainedLiveInfo);
        }
        if (metadata.retainedLiveInfo == nullptr && boundary > GetRegionStart()) {
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            NoteRetainedPreserve();
            return;
        }
        metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
        NoteRetainedPreserve();
    }

    ALWAYS_INLINE void PreserveRetainedLiveInfo(MAddress coveredUpToOverride)
    {
        if (coveredUpToOverride == GetRegionStart() && GetRegionAllocPtr() != GetRegionStart()) {
            CHECK(GetLiveByteCount() == 0);
            metadata.retainedLiveInfo = GetLiveInfo();
            metadata.retainedLiveInfoEpoch = GetSnapshotEpoch();
            metadata.retainedLiveInfoCoveredUpTo = coveredUpToOverride;
            metadata.retainedLiveInfoState = RetainedLiveInfoState::SNAPSHOT_VALID;
            NoteRetainedPreserve();
            return;
        }
        CHECK(coveredUpToOverride == GetRegionAllocPtr());
        PreserveRetainedLiveInfo();
    }

    // holderlive (F2): record the outcome of a Preserve* call. Called after the state word
    // is already written, so the op code is derived from it rather than duplicated.
    ALWAYS_INLINE void NoteRetainedPreserve()
    {
        ++metadata.retainedPreserveCnt;
        switch (metadata.retainedLiveInfoState) {
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
        if (metadata.retainedLiveInfoState == RetainedLiveInfoState::NEVER_EXAMINED) {
            return;
        }
        ++metadata.retainedClearCnt;
        metadata.retainedLastOp = static_cast<uint8_t>(op);
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
                allocatedLiveInfo->GetMarkFace<Generation::Young>().epoch = 0;
                allocatedLiveInfo->GetMarkFace<Generation::Young>().bitmap = nullptr;
                allocatedLiveInfo->GetMarkFace<Generation::Old>().epoch = 0;
                allocatedLiveInfo->GetMarkFace<Generation::Old>().bitmap = nullptr;
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
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return nullptr;
        }
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace<G>();
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
        LiveInfo::MarkFace& face = liveInfo->GetMarkFace<G>();
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
        // The large-region face is a single bit rather than a bitmap, but it is
        // still scoped by the same per-generation mark epoch.  A captured view
        // from an earlier cycle must not observe a later cycle's reused bit.
        if (view.GetEpoch() != GetMarkSnapshotEpoch<G>()) {
            return 0;
        }
        if (G == Generation::Young) {
            return (__atomic_load_n(&metadata.youngSnapshotEpoch, std::memory_order_acquire) &
                    YOUNG_LARGE_MARKED_BIT) != 0;
        }
        return metadata.regionStateBitField.GetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1) != 0;
    }

    template<Generation G>
    void SetMarkedRegionFlag(MarkView<G> view, uint8_t flag)
    {
        CHECK(view.GetRegion() == this);
        CHECK(view.GetEpoch() == GetMarkSnapshotEpoch<G>());
        if (G == Generation::Young) {
            if (flag != 0) {
                __atomic_fetch_or(&metadata.youngSnapshotEpoch, YOUNG_LARGE_MARKED_BIT, __ATOMIC_RELEASE);
            } else {
                __atomic_fetch_and(&metadata.youngSnapshotEpoch, YOUNG_SNAPSHOT_EPOCH_MASK, __ATOMIC_RELEASE);
            }
            return;
        }
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::MARKED_REGION_FLAG, 1, flag);
    }

    void ResetMarkBit(MarkView<Generation::Old> view)
    {
        SetMarkedRegionFlag(view, 0);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
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
        if (IsLargeRegion()) {
            if (GetMarkedRegionFlag(view) != 1) {
                SetMarkedRegionFlag(view, 1);
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
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::MarkObject");
        bool already = writeBm->MarkBits(offset, objSize, regionSize);
        if (!already) {
            AddLiveByteCount(objSize);
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized0", G);
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized0", G);
        CHECK(IsMarkedObject(view, offset));
        return already;
    }

    template<Generation G>
    bool MarkObject(MarkView<G> view, const BaseObject* obj, size_t objSize)
    {
        CHECK(view.GetRegion() == this);
        if (IsLargeRegion()) {
            if (GetMarkedRegionFlag(view) != 1) {
                SetMarkedRegionFlag(view, 1);
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
        RegionBitmap* writeBm = GetOrAllocMarkBitmap(view);
        SealCheck::NotePaint(this, offset, objSize, "RegionInfo::MarkObject_sized");
        bool already = writeBm->MarkBits(offset, objSize, regionSize);
        if (!already) {
            AddLiveByteCount(objSize);
        }
        (void)TagReuseProbe::NoteMarkBitsSticky(this, offset, true, "MarkObject_sized", G);
        (void)MarkWhyProbe::NoteAfterMarkBits(this, obj, offset, objSize, regionSize, writeBm, already,
                                              "MarkObject_sized", G);
        CHECK(IsMarkedObject(view, offset));
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
    template<Generation G>
    bool NoteMarkEpochOnRead(MarkView<G> view, LiveInfo* liveInfo)
    {
        CHECK(view.GetRegion() == this);
        if (liveInfo == nullptr) {
            return false;
        }
        LiveInfo::MarkFace& markFace = liveInfo->GetMarkFace<G>();
        RegionBitmap* bitmap = __atomic_load_n(&markFace.bitmap, std::memory_order_acquire);
        // A LiveInfo is now only the carrier for two lazy faces.  The other
        // generation may have allocated the carrier while this face has never
        // existed; absence is ordinary "unmarked", not a stale-face read.
        if (bitmap == nullptr || reinterpret_cast<MAddress>(bitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        const uint64_t face = markFace.epoch.load(std::memory_order_acquire);
        const uint64_t now = GetMarkSnapshotEpoch<G>();
        if (view.GetEpoch() == now && face == view.GetEpoch()) {
            return true;
        }
        EnsureMarkEpochAtexit();
        size_t n = markEpochStaleReadCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (MarkEpochAssertEnabled()) {
            LOG(RTLOG_FATAL,
                "[GCV2][mark-epoch] stale LiveInfo read generation=%s region=%p "
                "viewEpoch=%llu faceEpoch=%llu regionEpoch=%llu n=%zu "
                "env=MRT_GCV2_MARK_EPOCH_ASSERT=1",
                G == Generation::Young ? "young" : "old", this,
                static_cast<unsigned long long>(view.GetEpoch()), static_cast<unsigned long long>(face),
                static_cast<unsigned long long>(now), n);
        }
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
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1;
        }
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return false;
        }
        // markepoch §5: stale face ⇒ unmarked (ZGC is_marked false before bit test).
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace<G>().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        size_t offset = GetAddressOffset(reinterpret_cast<MAddress>(obj));
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
    bool IsMarkedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1;
        }
        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return false;
        }
        if (!NoteMarkEpochOnRead(view, liveInfo)) {
            return false;
        }
        RegionBitmap* markBitmap =
            __atomic_load_n(&liveInfo->GetMarkFace<G>().bitmap, std::memory_order_acquire);
        if (markBitmap == nullptr || reinterpret_cast<MAddress>(markBitmap) == LiveInfo::TEMPORARY_PTR) {
            return false;
        }
        return markBitmap->IsMarked(offset);
    }

    template<Generation G>
    bool IsSurvivedObject(MarkView<G> view, size_t offset)
    {
        CHECK(view.GetRegion() == this);
        if (IsLargeRegion()) {
            return GetMarkedRegionFlag(view) == 1 ||
                (G == Generation::Old && metadata.isResurrected == 1);
        }

        LiveInfo* liveInfo = GetLiveInfo();
        if (liveInfo == nullptr) {
            return false;
        }
        if (NoteMarkEpochOnRead(view, liveInfo)) {
            RegionBitmap* markBitmap =
                __atomic_load_n(&liveInfo->GetMarkFace<G>().bitmap, std::memory_order_acquire);
            if (markBitmap != nullptr && reinterpret_cast<MAddress>(markBitmap) != LiveInfo::TEMPORARY_PTR &&
                markBitmap->IsMarked(offset)) {
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

    static void Initialize(size_t nUnit, uintptr_t heapAddress)
    {
        UnitInfo::totalUnitCount = nUnit;
        UnitInfo::heapStartAddress = heapAddress;
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
        static_assert(sizeof(UnitInfo) == 200, "per-unit metadata size changed; it is per-page, so price it");
        if (RouteDestHold::AccountOn()) {
            LOG(RTLOG_ERROR,
                "[GCV2][routedest] unit_metadata sizeof_UnitInfo=%zu unit_size=%zu overhead_permille=%zu",
                sizeof(UnitInfo), static_cast<size_t>(UNIT_SIZE), (sizeof(UnitInfo) * 1000) / UNIT_SIZE);
        }
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
        HeldFreeDiag::NoteClearRange(unitAddress, size);
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

    // Sole mint of RouteTicket. Guard = survived on liveInfo0 AND object start.
    // MarkBits paints multi-byte ranges (LiveInfo.h MarkBits); IsSurvivedObject(interior)
    // is true for every 8B covered by a marked object, but VisitLive/Copy only run on
    // size-walk starts (RegionManager.cpp VisitLiveObjectsUntilFalse). Admit⊃Copy at
    // address granularity is the permanent hole (permrate/permwho). ROUTE_DOMAIN.md §0-②.
    // Start predicate = tip word looks like TypeInfo (same reject set as
    // PlausibleManagedObjectGate tip arm) — interiors carry field data, not TypeInfo*.
    // Miss = empty OptionalRouteTicket; never silent derive.
    // Anchor: ops/design/ROUTE_DOMAIN.md §2; former guard RegionInfo.h GetRoute.
    ATTR_WARN_UNUSED OptionalRouteTicket AdmitForRoute(BaseObject* fromObj)
    {
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        size_t offset = GetAddressOffset(fromAddress);
        LiveInfo* ghostLiveInfo = metadata.liveInfo0;
        bool survived = false;
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            survived = IsSurvivedObject(view, ghostLiveInfo, offset);
        } else {
            MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
            survived = IsSurvivedObject(view, ghostLiveInfo, offset);
        }
        // Large region: single object at start; tip check is the start test for small.
        bool startOk = false;
        if (survived) {
            if (IsLargeRegion()) {
                startOk = (offset == 0);
            } else if (fromObj != nullptr) {
                // Tip-only read (StateWord). No IsVaildType / GetSize — same shape as
                // Collector::PlausibleManagedObjectGate tip arm (Collector.cpp).
                TypeInfo* tip = fromObj->GetTypeInfo();
                uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
                constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x100000000ULL;
                if (tipAddr != 0 && tipAddr >= kMinPlausibleTypeInfoAddr &&
                    (tipAddr & StateWord::ADDRESS_ALIGN_MASK) == 0 &&
                    (tipAddr & 0xffffffffULL) != 0 &&
                    !Heap::IsHeapAddress(tipAddr)) {
                    startOk = true;
                }
                // uafclose: after CompactRegion copies survivors and memset free-tail, the
                // from header at the old address is zeroed. RouteState COMPACTED (and
                // FORWARDED after VisitLive) still needs Admit so FindToVersion can return
                // the geometric to — tip is no longer a valid start oracle. Survivor bit
                // on liveInfo0 is the domain membership proof (VisitLive already walked starts).
                if (!startOk) {
                    RouteState rsTip = GetRouteState();
                    if (rsTip == RouteState::COMPACTED || rsTip == RouteState::FORWARDED) {
                        startOk = true;
                    }
                }
            }
        }
        // Domain-eq probe (admitstart): MRT_GCV2_DOMAINEQ=1. Counts survived-but-not-start
        // (would-have-been-Admit under old multi-bit guard). Product fix rejects those.
        // Inject: MRT_GCV2_DOMAINEQ_INJECT=1 forces one synthetic interior count so a silent
        // harness is visible. Default off — zero product side effect.
        static const int domainEqMode = []() {
            const char* v = std::getenv("MRT_GCV2_DOMAINEQ");
            if (v != nullptr && v[0] == '1' && v[1] == '\0') {
                return 1;
            }
            return 0;
        }();
        if (domainEqMode != 0) {
            static std::atomic<size_t> g_deAdmitOk{ 0 };
            static std::atomic<size_t> g_deSurvivedNotStart{ 0 };
            static std::atomic<size_t> g_deInject{ 0 };
            static std::atomic<bool> g_deAtexit{ false };
            bool expect = false;
            if (g_deAtexit.compare_exchange_strong(expect, true, std::memory_order_relaxed)) {
                std::atexit([]() {
                    const char* inj = std::getenv("MRT_GCV2_DOMAINEQ_INJECT");
                    if (inj != nullptr && inj[0] == '1' && inj[1] == '\0') {
                        g_deInject.store(1, std::memory_order_relaxed);
                        g_deSurvivedNotStart.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::fprintf(stderr,
                                 "[GCV2][domaineq] admitOk=%zu survivedNotStart=%zu inject=%zu "
                                 "env=MRT_GCV2_DOMAINEQ=1\n",
                                 g_deAdmitOk.load(std::memory_order_relaxed),
                                 g_deSurvivedNotStart.load(std::memory_order_relaxed),
                                 g_deInject.load(std::memory_order_relaxed));
                    std::fflush(stderr);
                });
            }
            if (survived && startOk) {
                g_deAdmitOk.fetch_add(1, std::memory_order_relaxed);
            } else if (survived && !startOk) {
                g_deSurvivedNotStart.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!survived || !startOk) {
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
                            RegionBitmap* mb = GetRouteMarkGeneration() == Generation::Young
                                ? GetMarkBitmap(GetRouteMarkView<Generation::Young>(), ghostLiveInfo)
                                : GetMarkBitmap(GetRouteMarkView<Generation::Old>(), ghostLiveInfo);
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
                        if (GetRouteMarkGeneration() == Generation::Young) {
                            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
                            liveMarked = IsSurvivedObject(view, curLive, offset);
                            regionMarked = IsSurvivedObject(view, offset);
                        } else {
                            MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
                            liveMarked = IsSurvivedObject(view, curLive, offset);
                            regionMarked = IsSurvivedObject(view, offset);
                        }
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
                                if (GetRouteMarkGeneration() == Generation::Young && hr->IsYoungRegion()) {
                                    MarkView<Generation::Young> hostView = hr->GetMarkView<Generation::Young>();
                                    hostMarked = static_cast<unsigned>(hr->IsMarkedObject(hostView, hostObj));
                                } else {
                                    MarkView<Generation::Old> hostView = hr->GetMarkView<Generation::Old>();
                                    hostMarked = static_cast<unsigned>(hr->IsMarkedObject(hostView, hostObj));
                                }
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
    // Compacted: dest is the dense pack slot recorded by CompactRegion, not prefix-sum.
    BaseObject* GetRoute(RouteTicket t)
    {
        BaseObject* fromObj = t.From();
        MAddress fromAddress = reinterpret_cast<MAddress>(fromObj);
        CompactRouteTable* compactRouteTable = LoadCompactRouteTable();
        if (compactRouteTable != nullptr) {
            BaseObject* packed = LookupCompactRoute(GetAddressOffset(fromAddress), compactRouteTable);
            if (packed != nullptr) {
                return packed;
            }
            // Compacted and not packed: prefix-sum dest is a hole (dense pack).
            // Keep from if Compact left it in place (walk break); else no to-version.
            if (fromObj->IsValidObject()) {
                return fromObj;
            }
            return nullptr;
        }
        // FreeCompactRouteTable publishes NORMAL before detaching a compact table. An
        // already-admitted reader that loses the detach race must soft-miss rather than
        // reinterpret a compact destination as prefix-sum geometry.
        RouteState routeState = GetRouteState();
        if (routeState != RouteState::ROUTED && routeState != RouteState::FORWARDED) {
            return nullptr;
        }
        uint64_t preLiveBytes = GetPreLiveBytesInGhostRegion(fromAddress);
        MAddress toAddr = metadata.routeInfo.GetRoute(preLiveBytes);
        // routedom: observe mark-domain at geometric GetRoute call site (default off).
        if (RouteDom::Enabled()) {
            RouteDom::NoteRoute(this, fromObj, preLiveBytes, static_cast<uintptr_t>(toAddr));
        }
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
    }

    BaseObject* LookupCompactRoute(size_t fromOff, const CompactRouteTable* table) const
    {
        auto it = table->find(fromOff);
        if (it == table->end()) {
            return nullptr;
        }
        return from_region_addr(it->second);
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
        return metadata.routeInfo.GetRoute(preLiveBytes);
    }

    ZGenerationId generation_id() const { return metadata._generation_id; }

    template<Generation G>
    void PrepareForwardableRegion(MarkView<G> view)
    {
        CHECK(view.GetRegion() == this);
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
        SetRouteMarkGeneration(G);
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
        // Publish route retirement before detaching the table. A reader that observes
        // the atomic nullptr then also observes NORMAL and soft-misses in GetRoute.
        SetRouteState(NORMAL);
        FreeCompactRouteTable();
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
            NoteRetainedClear(RETAINED_OP_CLEAR_CHECKED);
            metadata.retainedLiveInfo = nullptr;
            // holderlive (F2): this unbind exists because the borrowed LiveInfo* is about to
            // dangle — it says nothing about whether the snapshot is still true. When we own
            // the bits, drop the pointer and keep the verdict.
            if (metadata.retainedMarkWords != nullptr) {
                return;
            }
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
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
        // Clear a large face while the supplied view still names the current
        // epoch; only then publish the epoch bump that makes old views stale.
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(view, 0);
            if (G == Generation::Old) {
                MarkView<Generation::Young> youngView(this, GetMarkSnapshotEpoch<Generation::Young>());
                SetMarkedRegionFlag(youngView, 0);
            }
        }
        BumpSnapshotEpochFromClearLiveInfo<G>();
        if (G == Generation::Old) {
            // A major cycle supersedes both closures.  Dropping the carrier is
            // safe because both per-generation faces live in the same arena.
            BumpMarkSnapshotEpoch<Generation::Young>();
            if (metadata.liveInfo != nullptr) {
                metadata.liveInfo = nullptr;
            }
        } else {
            // A minor starts a new young closure without erasing the last major
            // closure.  Only the young lazy face is detached.
            LiveInfo* liveInfo = GetLiveInfo();
            if (liveInfo != nullptr) {
                LiveInfo::MarkFace& youngFace = liveInfo->GetMarkFace<Generation::Young>();
                __atomic_store_n(&youngFace.bitmap, static_cast<RegionBitmap*>(nullptr),
                                 std::memory_order_release);
                youngFace.epoch = 0;
            }
        }
        // Same carrier rule as liveInfo/retained: mark-cycle start drops ghost too.
        // PrepareForwardableRegion copies liveInfo→liveInfo0; without this, liveInfo0
        // can outlive ReleaseMemory(previous tag) (tagreuse T2).
        metadata.liveInfo0 = nullptr;
        if (G == Generation::Old) {
            NoteRetainedClear(RETAINED_OP_CLEAR_ALL);
            // A new major mark supersedes the retained major snapshot.  Young
            // clears deliberately leave this old/major authority intact.
            FreeRetainedMarkWords();
            metadata.retainedLiveInfo = nullptr;
            metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
            metadata.retainedLiveInfoEpoch = 0;
            metadata.retainedLiveInfoCoveredUpTo = 0;
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
            __atomic_store_n(&metadata.liveInfo, static_cast<LiveInfo*>(nullptr), std::memory_order_release);
            // Tracking phase for this LiveInfo ended with its backing store about to vanish.
            __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        }
        if (inRange(metadata.liveInfo0)) {
            metadata.liveInfo0 = nullptr;
        }
        if (inRange(metadata.retainedLiveInfo)) {
            NoteRetainedClear(RETAINED_OP_CLEAR_RANGE);
            metadata.retainedLiveInfo = nullptr;
            // holderlive (F2): same rule as CheckAndClearLiveInfo — the range is about to be
            // madvise'd, so the pointer must go; an owned copy is not in that range.
            if (metadata.retainedMarkWords != nullptr) {
                return;
            }
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
    // routedest: destination-side hold. Idempotent by construction, and it has to be:
    // RouteOrCompactRegionImpl publishes the split plan twice for the same holder when the
    // toRegion2 allocation fails (SetRouteInfo at RegionManager.cpp:1992, then again at
    // :1999), so the stamp on toRegion1 runs twice. Stamping a byte twice is a no-op.
    // Do NOT turn this into a reference count without handling that double publish.
    void SetRouteDestHold(uint8_t flag)
    {
        __atomic_store_n(&metadata.routeDestHold, flag, __ATOMIC_RELEASE);
    }
    bool IsRouteDestHeld() const
    {
        return __atomic_load_n(&metadata.routeDestHold, __ATOMIC_ACQUIRE) != 0;
    }
    void SetInGhostRegion(uint8_t flag)
    {
        metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
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

    // genface: promotion is the ownership boundary between the two mark faces.
    // A caller must name the Young closure it is retiring; the returned Old
    // view observes only the independently maintained Old face.  In particular,
    // this operation deliberately does not copy either the ordinary Young bitmap
    // or the Young large-region flag into the Old face.
    MarkView<Generation::Old> PromoteYoungRegion(MarkView<Generation::Young> youngView)
    {
        CHECK_DETAIL(youngView.GetRegion() == this, "young promotion view belongs to another region");
        CHECK_DETAIL(IsYoungRegion(), "cannot promote an old region %p", this);
        CHECK_DETAIL(youngView.GetEpoch() == GetMarkSnapshotEpoch<Generation::Young>(),
                     "cannot promote region %p through a stale young mark view", this);
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
    bool IsKnownEmpty(MarkView<Generation::Old> view) const
    {
        CHECK(view.GetRegion() == this);
        uint64_t raw = __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
        const bool auth = (raw & LIVE_AUTHORITY_BIT) != 0;
        bool emptyByEpoch = false;
        if (IsLargeRegion()) {
            emptyByEpoch = GetMarkedRegionFlag(view) == 0;
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr || reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
                emptyByEpoch = true;
            } else if (liveInfo->GetMarkFace<Generation::Old>().epoch.load(std::memory_order_acquire) !=
                           view.GetEpoch() ||
                       view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Old>()) {
                // markepoch: stale face ⇒ unmarked (ZGC is_marked false before bit test).
                emptyByEpoch = true;
            } else {
                // Current-cycle mark face present ⇒ ZGC is_marked true ⇒ not empty for reclaim.
                emptyByEpoch = false;
            }
        }
        // oneseq: authority vs epoch-empty divergence (const path uses relaxed atomics only).
        if (OneseqDiagEnabled()) {
            oneseqIsKnownEmptyCalls.fetch_add(1, std::memory_order_relaxed);
            if (!auth && emptyByEpoch) {
                oneseqAuthBlocksReclaim.fetch_add(1, std::memory_order_relaxed);
            } else if (auth && emptyByEpoch) {
                oneseqAuthAndEmpty.fetch_add(1, std::memory_order_relaxed);
            } else if (auth && !emptyByEpoch) {
                oneseqAuthNotEmpty.fetch_add(1, std::memory_order_relaxed);
            } else {
                oneseqNoAuthNotEmpty.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!auth) {
            return false;
        }
        return emptyByEpoch;
    }

    bool IsKnownYoungEmpty(MarkView<Generation::Young> view) const
    {
        CHECK(view.GetRegion() == this);
        uint64_t raw = __atomic_load_n(&metadata.liveByteCount, std::memory_order_acquire);
        const bool auth = (raw & LIVE_AUTHORITY_BIT) != 0;
        bool emptyByEpoch = false;
        if (IsLargeRegion()) {
            emptyByEpoch = GetMarkedRegionFlag(view) == 0;
        } else {
            LiveInfo* liveInfo = __atomic_load_n(&metadata.liveInfo, std::memory_order_acquire);
            if (liveInfo == nullptr || reinterpret_cast<MAddress>(liveInfo) == LiveInfo::TEMPORARY_PTR) {
                emptyByEpoch = true;
            } else {
                emptyByEpoch = liveInfo->GetMarkFace<Generation::Young>().epoch.load(std::memory_order_acquire) !=
                                   view.GetEpoch() ||
                    view.GetEpoch() != GetMarkSnapshotEpoch<Generation::Young>();
            }
        }
        return auth && emptyByEpoch;
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
        __atomic_store_n(&metadata.liveByteCount, LIVE_AUTHORITY_BIT, std::memory_order_release);
        if (IsLargeRegion()) {
            SetMarkedRegionFlag(view, 0);
        }
        BumpSnapshotEpochFromResetAfterForward<G>();
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
            ? IsKnownYoungEmpty(MarkView<Generation::Young>(this, view.GetEpoch()))
            : IsKnownEmpty(MarkView<Generation::Old>(this, view.GetEpoch()));
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
        DCHECK(metadata.liveInfo0 != nullptr);
        size_t offset = GetAddressOffset(address);
        if (GetRouteMarkGeneration() == Generation::Young) {
            MarkView<Generation::Young> view = GetRouteMarkView<Generation::Young>();
            return metadata.liveInfo0->GetPreLiveBytes(view, offset, GetGhostRegionSize());
        }
        MarkView<Generation::Old> view = GetRouteMarkView<Generation::Old>();
        return metadata.liveInfo0->GetPreLiveBytes(view, offset, GetGhostRegionSize());
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
    static constexpr uint64_t YOUNG_LARGE_MARKED_BIT = static_cast<uint64_t>(1) << 63;
    static constexpr uint64_t YOUNG_SNAPSHOT_EPOCH_MASK = YOUNG_LARGE_MARKED_BIT - 1;
    static constexpr uint8_t MARK_FACE_SEALED_BIT = 1U << 0;
    static constexpr uint8_t ROUTE_MARK_YOUNG_BIT = 1U << 1;
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
        // holderlive (F2): owned copy of the retained mark bits (mark | resurrect). Null unless
        // MRT_GCV2_RETAINED_OWN_COPY=1. Freed by ClearLiveInfo / InitRegionInfo.
        uint64_t* retainedMarkWords = nullptr;
        uint32_t retainedMarkWordCnt = 0;

        // resolveto: Compact packs densely; GetRoute prefix-sum dests are holes.
        // Table maps from-offset → actual dest for COMPACTED regions only.
        void* compactRouteTable = nullptr;

        uintptr_t regionEnd0;
        RouteInfo routeInfo;
        uint64_t snapshotEpoch = 0;
        uint64_t youngSnapshotEpoch = 0;

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
        void SetInGhostRegion(uint8_t flag)
        {
            metadata.regionStateBitField.SetAtomicValue(RegionStateBitPos::IN_GHOST_FROM_REGION_FLAG, 1, flag);
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
        // See DispelGhostFromRegion: retire the route before detaching its compact table.
        SetRouteState(NORMAL);
        metadata.allocPtr = GetRegionStart();
        metadata.regionEnd = metadata.allocPtr + nUnit * RegionInfo::UNIT_SIZE;
        metadata.prevRegionIdx = NULLPTR_IDX;
        metadata.nextRegionIdx = NULLPTR_IDX;
        metadata.censusBoundaryOffset = 0;
        __atomic_store_n(&metadata.liveByteCount, 0, std::memory_order_release);
        metadata.liveInfo = nullptr;
        metadata.liveInfo0 = nullptr;
        FreeCompactRouteTable();
        FreeRetainedMarkWords();
        metadata.retainedLiveInfo = nullptr;
        metadata.retainedLiveInfoState = RetainedLiveInfoState::NEVER_EXAMINED;
        metadata.retainedLiveInfoEpoch = 0;
        metadata.retainedLiveInfoCoveredUpTo = 0;
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
        RouteDestHold::NoteReuse(this, IsRouteDestHeld());
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
        metadata.routeInfo.SetRouteInfo(0);
        // Ghost lives in unit metadata, not payload: ClearUnits cannot clear it.
        // TakeRegion reuses garbage without DispelGhostFromRegion (RegionInfo.h:667-698).
        SetInGhostRegion(0);
        SetOldMarkedRegionFlag(0);
        __atomic_fetch_and(&metadata.youngSnapshotEpoch, YOUNG_SNAPSHOT_EPOCH_MASK, __ATOMIC_RELEASE);
        SetEnqueuedRegionFlag(0);
        SetResurrectedRegionFlag(0);
        SetYoungRegionFlag(0);
        SetMarkFaceSealed(false);
        SetRouteMarkGeneration(Generation::Old);
        __atomic_store_n(&metadata.rawPointerObjectCount, 0, __ATOMIC_SEQ_CST);
        SetUnitRole(uClass);
    }

    void InitRegion(size_t nUnit, UnitRole uClass)
    {
        InitRegionInfo(nUnit, uClass);
        TlRawDiag::NoteInitRegion(this);
        RegionLifeDiag::NoteTake(this);

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
