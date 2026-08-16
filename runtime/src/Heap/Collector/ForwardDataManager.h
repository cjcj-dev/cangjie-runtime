// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FORWARD_DATA_MANAGER_H
#define MRT_FORWARD_DATA_MANAGER_H

#include "Base/ImmortalWrapper.h"
#include "Heap/Heap.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/mman.h>
#endif
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/MemUtils.h"
#include "Base/SysCall.h"
#include "Heap/Verify/FwdInflight.h"
#include "LiveInfo.h"

#ifdef _WIN64
#include "Base/AtomicSpinLock.h"
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

namespace MapleRuntime {

class ForwardDataManager {
    class ForwardDataSpace {
    public:
        struct Zone {
            enum ZoneType : size_t {
                LIVE_INFO,
                BIT_MAP,
                TOTAL_NUM,
            };
            uintptr_t zoneStartAddress = 0;
            std::atomic<uintptr_t> zonePosition;
        };
        ForwardDataSpace() = default;
        void InitializeMemory(uintptr_t start, size_t sz, size_t unitCount)
        {
            startAddress = start;
            size = sz;
            InitZones(unitCount);
        }
        void InitZones(size_t unitCount)
        {
            uintptr_t start = startAddress;
            allocZone[Zone::ZoneType::LIVE_INFO].zoneStartAddress = start;
            allocZone[Zone::ZoneType::LIVE_INFO].zonePosition = start;
#if defined(_WIN64)
            lastCommitEndAddr[Zone::ZoneType::LIVE_INFO].store(start);
#endif
            start += unitCount * sizeof(LiveInfo);
            allocZone[Zone::ZoneType::BIT_MAP].zoneStartAddress = start;
            allocZone[Zone::ZoneType::BIT_MAP].zonePosition = start;
#if defined(_WIN64)
            lastCommitEndAddr[Zone::ZoneType::BIT_MAP].store(start);
#endif
        }
        uintptr_t Allocate(Zone::ZoneType type, size_t sz)
        {
#if defined(_WIN64)
            allocSpinLock.Lock();
            uintptr_t startAddr = allocZone[type].zonePosition.fetch_add(sz);
            uintptr_t endAddr = startAddr + sz;
            uintptr_t lastAddr = lastCommitEndAddr[type].load(std::memory_order_relaxed);
            if (endAddr <= lastAddr) {
                allocSpinLock.Unlock();
                return startAddr;
            }
            size_t pageSize = RoundUp(sz, MapleRuntime::MRT_PAGE_SIZE);
            CHECK_E(UNLIKELY(!VirtualAlloc(reinterpret_cast<void*>(lastAddr), pageSize, MEM_COMMIT, PAGE_READWRITE)),
                    "VirtualAlloc commit failed in Allocate, errno: %d", GetLastError());
            lastCommitEndAddr[type].store(lastAddr + pageSize);
            allocSpinLock.Unlock();
            return startAddr;
#else
            return allocZone[type].zonePosition.fetch_add(sz);
#endif
        }
        void ReleaseMemory()
        {
#if defined(_WIN64)
            CHECK_E(UNLIKELY(!VirtualFree(reinterpret_cast<void*>(startAddress), size, MEM_DECOMMIT)),
                    "VirtualFree failed in ReturnPage, errno: %s", GetLastError());
#elif defined(__APPLE__)
            MapleRuntime::MemorySet(startAddress, size, 0, size);
            void* ret = mmap(reinterpret_cast<void*>(startAddress), size,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1 , 0);
            if (ret == MAP_FAILED) {
                LOG(RTLOG_ERROR, "forwarding fata mmap ixed failed");
            } else if (ret != reinterpret_cast<void*>(startAddress)) {
                LOG(RTLOG_ERROR, "mmap fixed at wrong addr %p -> %p", startAddress, ret);
            }
#else
            if (madvise(reinterpret_cast<void*>(startAddress), size, MADV_DONTNEED) == 0) {
                DLOG(REGION, "release forward-data @[%#zx+%zu, %#zx)", startAddress, size, startAddress + size);
            } else {
                MapleRuntime::MemorySet(startAddress, size, 0, size);
                DLOG(REGION, "clear forward-data @[%#zx+%zu, %#zx)", startAddress, size, startAddress + size);
            }
#endif
            for (size_t i = Zone::ZoneType::LIVE_INFO; i < Zone::ZoneType::TOTAL_NUM; ++i) {
                allocZone[i].zonePosition = allocZone[i].zoneStartAddress;
#if defined(_WIN64)
                lastCommitEndAddr[i].store(allocZone[i].zoneStartAddress);
#endif
            }
        }

        void UnbindPreviousLiveInfo();

        uintptr_t GetStartAddress() const { return startAddress; }
        size_t GetSize() const { return size; }
        uintptr_t GetZoneStart(Zone::ZoneType type) const { return allocZone[type].zoneStartAddress; }
        uintptr_t GetZonePos(Zone::ZoneType type) const { return allocZone[type].zonePosition.load(); }

    private:
        Zone allocZone[Zone::TOTAL_NUM];
        uintptr_t startAddress = 0;
        size_t size = 0;
#if defined(_WIN64)
        std::atomic<uintptr_t> lastCommitEndAddr[Zone::TOTAL_NUM];
        AtomicSpinLock allocSpinLock;
#endif
    };

public:
    ForwardDataManager() = default;
    ~ForwardDataManager()
    {
#ifdef _WIN64
        if (!VirtualFree(reinterpret_cast<void*>(forwardDataStart), 0, MEM_RELEASE)) {
            LOG(RTLOG_ERROR, "VirtualFree error for ForwardDataManager");
        }
#else
        if (munmap(reinterpret_cast<void*>(forwardDataStart), forwardDataSize) != 0) {
            LOG(RTLOG_ERROR, "munmap error for ForwardDataManager");
        }
#endif
    }

    static ForwardDataManager& GetForwardDataManager();

    void InitializeForwardData();

    void ClearPreviousForwardData();

    RegionBitmap* AllocateRegionBitmap(size_t regionSize)
    {
        uintptr_t addr = liveInfoData[currentTagID].Allocate(ForwardDataSpace::Zone::ZoneType::BIT_MAP,
                                                             RegionBitmap::GetRegionBitmapSize(regionSize));
        RegionBitmap* bitmap = reinterpret_cast<RegionBitmap*>(addr);
        CHECK(bitmap != nullptr);
        new (bitmap) RegionBitmap(regionSize);
        return bitmap;
    }

    LiveInfo* AllocateLiveInfo()
    {
        return reinterpret_cast<LiveInfo*>(
            liveInfoData[currentTagID].Allocate(ForwardDataSpace::Zone::ZoneType::LIVE_INFO, sizeof(LiveInfo)));
    }

    uint16_t GetPreviousTagID() const
    {
        return static_cast<uint16_t>((currentTagID + TAG_ID_COUNT - 1) % TAG_ID_COUNT);
    }

    // The ring rotates here: `id` is about to start receiving LiveInfo and bitmap
    // allocations, so a deferred release still owed on that slot has to land first. This is
    // the backstop that makes the grace period safe no matter how few phase transitions the
    // cycle happened to contain -- with the gate off it is a no-op.
    void SetTagID(uint16_t id)
    {
        FlushGraceSlot(id);
        currentTagID = id;
    }

    // fwdgrace -------------------------------------------------------------------------
    //
    // ZGC drains readers off a from-page before the page goes away: ZForwarding::detach_page
    // (zForwarding.cpp:171-181) is `while (_ref_count.load_acquire() != 0) _ref_lock.wait();`
    // and only then free_page. Our arena release did no draining at all.
    // ClearPreviousForwardData nulls the region liveInfo fields and then madvises the range,
    // and the comment there calls that a "Structural guarantee: no region field may still
    // address the range about to be madvise'd."
    //
    // Nulling a field is not a drain. AdmitForRoute (RegionInfo.h:1494) copies liveInfo0 into
    // a local before it uses it; nulling the field afterwards does not reach that local, and
    // the thread holding it goes on to read a range that MADV_DONTNEED has turned back into
    // zero pages. Same for the bitmap the LiveInfo points at.
    //
    // The wait used here is the one this repo already has, not a new one: RegionInfo::
    // AdvanceCompactRouteTableGracePeriod (RegionInfo.h:1822-1843) retires a detached
    // CompactRouteTable and frees it only after two completed phase transitions, on the
    // stated premise that a phase transition is a mutator grace period. The arena is retired
    // the same way and off the same edge (TracingCollector::TransitionToGCPhase). Only the
    // madvise is deferred -- the nulling stays where it was, because that is what stops NEW
    // readers from entering while the grace period covers the ones already inside.
    //
    // Bound, honestly stated: this is a grace period, not ZGC's blocking drain. It covers a
    // reader that a completed mutator phase transition covers. A reader that outlives two
    // transitions is not covered, and neither is a thread that does not take part in the
    // mutator handshake at all. Route B (a real _ref_count/_ref_lock pair on the arena) is
    // what closes that; this closes the part the repo already knows how to close.
    //
    // Gate: MRT_GCV2_FWDDATA_GRACE=1, default off. Off, ClearPreviousForwardData runs the
    // byte-for-byte previous sequence and none of the state below is ever touched.
    static bool GraceEnabled()
    {
        static const bool on = []() {
            const char* v = std::getenv("MRT_GCV2_FWDDATA_GRACE");
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        return on;
    }

    // One completed phase transition. Called from the same two sites that advance the
    // compact-route-table grace period, so the two cannot drift apart.
    static void AdvanceGracePeriod()
    {
        if (!GraceEnabled()) {
            return;
        }
        GetForwardDataManager().AdvanceGracePeriodImpl();
    }

private:
    void RetireGraceSlot(uint16_t slot)
    {
        std::lock_guard<std::mutex> lock(graceMutex);
        graceRetired.fetch_add(1, std::memory_order_relaxed);
        if (gracePending[slot]) {
            // Keep the earlier deadline. Minor GCs retire the same slot over and over
            // (only a major flips the tag), and refreshing the generation on each of them
            // would let a steady stream of minors postpone the release indefinitely -- a
            // wait that never ends is not a wait, it is a leak.
            return;
        }
        gracePending[slot] = true;
        gracePendingGeneration[slot] = graceGeneration.load(std::memory_order_relaxed);
    }

    void AdvanceGracePeriodImpl()
    {
        uint16_t ready[TAG_ID_COUNT];
        size_t readyCount = 0;
        {
            std::lock_guard<std::mutex> lock(graceMutex);
            const uint64_t generation = graceGeneration.load(std::memory_order_relaxed) + 1;
            graceGeneration.store(generation, std::memory_order_relaxed);
            for (uint16_t i = 0; i < TAG_ID_COUNT; ++i) {
                // Two completed transitions, exactly as RegionInfo.h:1832: a retire that
                // raced the boundary is then assigned to either side without endangering
                // a reader.
                if (gracePending[i] && (generation - gracePendingGeneration[i]) >= 2) {
                    gracePending[i] = false;
                    ready[readyCount] = i;
                    ++readyCount;
                }
            }
        }
        for (size_t i = 0; i < readyCount; ++i) {
            ReleaseSlotNow(ready[i], /*forced=*/false);
        }
    }

    void FlushGraceSlot(uint16_t slot)
    {
        if (!GraceEnabled()) {
            return;
        }
        bool owed = false;
        {
            std::lock_guard<std::mutex> lock(graceMutex);
            if (gracePending[slot]) {
                gracePending[slot] = false;
                owed = true;
            }
        }
        if (owed) {
            ReleaseSlotNow(slot, /*forced=*/true);
        }
    }

    void ReleaseSlotNow(uint16_t slot, bool forced)
    {
        ForwardDataSpace& space = liveInfoData[slot];
        // Counted at the instant the memory actually goes away. That is the point of moving
        // the call: with the gate on this is the deferred instant, so a non-zero count here
        // says the wait did not outlast the readers, and a zero count says it did (or that
        // there were none -- which is why the run needs its control arm).
        FwdInflight::NoteRetireGlobal(space.GetStartAddress(), space.GetSize(),
                                      FwdInflight::Retire::ARENA_RELEASE);
        space.ReleaseMemory();
        size_t released = graceReleased.fetch_add(1, std::memory_order_relaxed) + 1;
        if (forced) {
            graceForced.fetch_add(1, std::memory_order_relaxed);
        }
        VLOG(REPORT,
             "[GCV2][fwdgrace] release slot=%u forced=%u generation=%llu retired=%zu released=%zu "
             "forced_total=%zu",
             static_cast<unsigned>(slot), static_cast<unsigned>(forced),
             static_cast<unsigned long long>(graceGeneration.load(std::memory_order_relaxed)),
             graceRetired.load(std::memory_order_relaxed), released,
             graceForced.load(std::memory_order_relaxed));
    }

public:
    // Recycle the slot that just left the one-generation window (same timing as N=2).
    void UnbindPreviousLiveInfo() { liveInfoData[GetPreviousTagID()].UnbindPreviousLiveInfo(); }

private:
    size_t GetLiveInfoDataSize(size_t heapSize)
    {
        const size_t REGION_UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE; // must be equal to RegionInfo::UNIT_SIZE
        heapSize = RoundUp<size_t>(heapSize, REGION_UNIT_SIZE);
        size_t unitCnt = heapSize / REGION_UNIT_SIZE;
        regionUnitCount = unitCnt;
        // 64: bitmap 1 bit marks the 64 bits in region.
        constexpr uint8_t bitMarksSize = 64;
        // 4 bitmaps for each region: young mark, old mark, resurrect, enqueue.
        constexpr uint8_t bitmapNum = 4;
        return unitCnt * sizeof(LiveInfo) +
            unitCnt * (sizeof(RegionBitmap) + (REGION_UNIT_SIZE / bitMarksSize)) * bitmapNum;
    }
    ForwardDataSpace liveInfoData[TAG_ID_COUNT];
    size_t regionUnitCount = 0;
    uintptr_t forwardDataStart = 0;
    size_t forwardDataSize = 0;
    uint16_t currentTagID = 0; // propagate from collector.

    // fwdgrace state. Reached only when the gate is on; the retire/advance/flush edges all
    // early-return before touching it otherwise. Mutex rather than lock-free because every
    // edge is a phase transition or a GC-thread step -- the compact-route-table retire list
    // it mirrors takes a mutex on the same edges (RegionInfo.h:2611-2632).
    std::mutex graceMutex;
    // Written only under graceMutex; atomic because ReleaseSlotNow logs it from outside the
    // lock and a torn/racy plain read there would be a data race for a log line.
    std::atomic<uint64_t> graceGeneration{ 0 };
    bool gracePending[TAG_ID_COUNT] = {};
    uint64_t gracePendingGeneration[TAG_ID_COUNT] = {};
    std::atomic<size_t> graceRetired{ 0 };
    std::atomic<size_t> graceReleased{ 0 };
    std::atomic<size_t> graceForced{ 0 };
};
} // namespace MapleRuntime
#endif // MRT_FORWARD_DATA_MANAGER_H
