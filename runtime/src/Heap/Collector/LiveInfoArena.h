// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_LIVE_INFO_ARENA_H
#define MRT_LIVE_INFO_ARENA_H

#include "Base/ImmortalWrapper.h"
#include "Heap/Heap.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/mman.h>
#endif
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/MemUtils.h"
#include "Base/SysCall.h"
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

class LiveInfoArena {
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
    LiveInfoArena() = default;
    ~LiveInfoArena()
    {
#ifdef _WIN64
        if (!VirtualFree(reinterpret_cast<void*>(forwardDataStart), 0, MEM_RELEASE)) {
            LOG(RTLOG_ERROR, "VirtualFree error for live-info arena");
        }
#else
        if (munmap(reinterpret_cast<void*>(forwardDataStart), forwardDataSize) != 0) {
            LOG(RTLOG_ERROR, "munmap error for live-info arena");
        }
#endif
    }

    static LiveInfoArena& GetLiveInfoArena();

    void InitializeForwardData();

    RegionBitmap* AllocateRegionBitmap(size_t regionSize)
    {
        if (RegionBitmap* recycled = TakeRecycledRegionBitmap(regionSize)) {
            new (recycled) RegionBitmap(regionSize);
            recycled->Reset();
            return recycled;
        }
        uintptr_t addr = liveInfoData.Allocate(ForwardDataSpace::Zone::ZoneType::BIT_MAP,
                                               RegionBitmap::GetRegionBitmapSize(regionSize));
        RegionBitmap* bitmap = reinterpret_cast<RegionBitmap*>(addr);
        CHECK(bitmap != nullptr);
        new (bitmap) RegionBitmap(regionSize);
        return bitmap;
    }

    void RecycleRegionBitmap(RegionBitmap* bitmap)
    {
        if (bitmap == nullptr) {
            return;
        }
        const size_t regionSize = bitmap->CoveredRegionSize();
        std::lock_guard<std::mutex> guard(recycleMutex);
        recycledBySize[regionSize].push_back(bitmap);
    }

    RegionBitmap* PublishMatchingBitmap(RegionBitmap** slot, RegionBitmap* current, size_t regionSize)
    {
        if (current->CoversRegionSize(regionSize)) {
            return current;
        }
        RegionBitmap* replacement = AllocateRegionBitmap(regionSize);
        RegionBitmap* expected = current;
        if (__atomic_compare_exchange_n(slot, &expected, replacement, false, std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
            RecycleRegionBitmap(current);
            return replacement;
        }
        RecycleRegionBitmap(replacement);
        return expected;
    }

    LiveInfo* AllocateLiveInfo()
    {
        return reinterpret_cast<LiveInfo*>(
            liveInfoData.Allocate(ForwardDataSpace::Zone::ZoneType::LIVE_INFO, sizeof(LiveInfo)));
    }

private:
    RegionBitmap* TakeRecycledRegionBitmap(size_t regionSize)
    {
        std::lock_guard<std::mutex> guard(recycleMutex);
        auto it = recycledBySize.find(regionSize);
        if (it == recycledBySize.end() || it->second.empty()) {
            return nullptr;
        }
        RegionBitmap* bitmap = it->second.back();
        it->second.pop_back();
        return bitmap;
    }

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
    ForwardDataSpace liveInfoData;
    size_t regionUnitCount = 0;
    uintptr_t forwardDataStart = 0;
    size_t forwardDataSize = 0;
    std::mutex recycleMutex;
    std::unordered_map<size_t, std::vector<RegionBitmap*>> recycledBySize;
};
} // namespace MapleRuntime
#endif // MRT_LIVE_INFO_ARENA_H
