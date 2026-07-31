// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/ImmortalWrapper.h"
#include "Heap/Allocator/RegionInfo.h"
#include "LiveInfo.h"
#include "ForwardDataManager.h"

namespace MapleRuntime {

static ImmortalWrapper<ForwardDataManager> forwardDataManager;
ForwardDataManager& ForwardDataManager::GetForwardDataManager() { return *forwardDataManager; }

void ForwardDataManager::InitializeForwardData()
{
    size_t maxHeapBytes = Heap::GetHeap().GetMaxCapacity();
    size_t requiredLiveInfoSize = GetLiveInfoDataSize(maxHeapBytes);
    size_t liveInfoSize = RoundUp(requiredLiveInfoSize, MapleRuntime::MRT_PAGE_SIZE);
    // 2: forwadData is the twice size of liveInfo
    forwardDataSize = liveInfoSize * 2;

#ifdef _WIN64
    void* startAddress = VirtualAlloc(NULL, forwardDataSize, MEM_RESERVE, PAGE_READWRITE);
    if (startAddress == NULL) {
        LOG(RTLOG_FATAL, "failed to initialize ForwardDataManager");
    }
#else
    void* startAddress = mmap(nullptr, forwardDataSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (startAddress == MAP_FAILED) {
        LOG(RTLOG_FATAL, "failed to initialize ForwardDataManager");
    } else {
#ifndef __APPLE__
        (void)madvise(startAddress, forwardDataSize, MADV_NOHUGEPAGE);
        MRT_PRCTL(startAddress, forwardDataSize, "forward_data");
#endif
    }
#endif

    forwardDataStart = reinterpret_cast<uintptr_t>(startAddress);
    const char* zoneWaterEnv = std::getenv("MRT_ZONEWATER");
    zoneWaterEnabled = zoneWaterEnv != nullptr && std::strcmp(zoneWaterEnv, "1") == 0;
    liveInfoData[0].InitializeMemory(forwardDataStart, liveInfoSize, regionUnitCount, zoneWaterEnabled);
    liveInfoData[1].InitializeMemory(forwardDataStart + liveInfoSize, liveInfoSize, regionUnitCount, zoneWaterEnabled);
    if (zoneWaterEnabled) {
        std::fprintf(stderr,
            "[ZONEWATER-MAP] formula=%zu mapped=%zu rounding_padding=%zu unit_count=%zu "
            "formula_eq_mapped=%u\n",
            requiredLiveInfoSize, liveInfoSize, liveInfoSize - requiredLiveInfoSize, regionUnitCount,
            static_cast<unsigned>(requiredLiveInfoSize == liveInfoSize));
        for (size_t tag = 0; tag < 2; ++tag) {
            std::fprintf(stderr,
                "[ZONEWATER-INIT] tag=%zu LI=[%#zx,%#zx) quota=%zu BM=[%#zx,%#zx) quota=%zu tag_len=%zu\n",
                tag,
                liveInfoData[tag].GetZoneStartAddress(ForwardDataSpace::Zone::ZoneType::LIVE_INFO),
                liveInfoData[tag].GetZoneEndAddress(ForwardDataSpace::Zone::ZoneType::LIVE_INFO),
                liveInfoData[tag].GetZoneCapacity(ForwardDataSpace::Zone::ZoneType::LIVE_INFO),
                liveInfoData[tag].GetZoneStartAddress(ForwardDataSpace::Zone::ZoneType::BIT_MAP),
                liveInfoData[tag].GetZoneEndAddress(ForwardDataSpace::Zone::ZoneType::BIT_MAP),
                liveInfoData[tag].GetZoneCapacity(ForwardDataSpace::Zone::ZoneType::BIT_MAP),
                liveInfoData[tag].GetSpaceSize());
        }
        std::fflush(stderr);
    }
}

void ForwardDataManager::ReportZoneWaterAndReset(size_t minorsInTag)
{
    if (!zoneWaterEnabled) {
        return;
    }
    // Report the tag that just finished (currentTagID before FlipTagID).
    ForwardDataSpace& data = liveInfoData[currentTagID];
    ForwardDataSpace::ZoneWaterSnapshot snapshot = data.GetZoneWaterSnapshot(true);
    constexpr auto liveInfoZone = ForwardDataSpace::Zone::ZoneType::LIVE_INFO;
    constexpr auto bitMapZone = ForwardDataSpace::Zone::ZoneType::BIT_MAP;
    for (size_t i = liveInfoZone; i < ForwardDataSpace::Zone::ZoneType::TOTAL_NUM; ++i) {
        zoneWaterAllocationCount[i] += snapshot.allocationCount[i];
        if (snapshot.highWater[i] > zoneWaterHighWater[i]) {
            zoneWaterHighWater[i] = snapshot.highWater[i];
        }
    }
    size_t liveInfoQuota = data.GetZoneCapacity(liveInfoZone);
    size_t bitMapQuota = data.GetZoneCapacity(bitMapZone);
    size_t tagLen = data.GetSpaceSize();
    size_t combined = snapshot.highWater[liveInfoZone] + snapshot.highWater[bitMapZone];
    double liveInfoPercent = liveInfoQuota == 0 ? 0.0 :
        100.0 * static_cast<double>(snapshot.highWater[liveInfoZone]) / static_cast<double>(liveInfoQuota);
    double bitMapPercent = bitMapQuota == 0 ? 0.0 :
        100.0 * static_cast<double>(snapshot.highWater[bitMapZone]) / static_cast<double>(bitMapQuota);
    unsigned overQuota = (snapshot.highWater[liveInfoZone] > liveInfoQuota ||
                          snapshot.highWater[bitMapZone] > bitMapQuota) ? 1u : 0u;
    unsigned overZone = combined > tagLen ? 1u : 0u;
    if (overQuota != 0) {
        zoneWaterEverOverQuota = 1;
    }
    if (overZone != 0) {
        zoneWaterEverOverZone = 1;
    }
    std::fprintf(stderr,
        "[ZONEWATER] tag=%u minors_in_tag=%zu LI_uses=%zu LI_hw=%zu/%zu LI_pct=%.6f%% "
        "BM_uses=%zu BM_hw=%zu/%zu BM_pct=%.6f%% OVER_QUOTA=%u OVER_ZONE=%u\n",
        static_cast<unsigned>(currentTagID), minorsInTag,
        snapshot.allocationCount[liveInfoZone], snapshot.highWater[liveInfoZone], liveInfoQuota, liveInfoPercent,
        snapshot.allocationCount[bitMapZone], snapshot.highWater[bitMapZone], bitMapQuota, bitMapPercent,
        overQuota, overZone);
    std::fflush(stderr);
}

void ForwardDataManager::ReportFinalZoneWater()
{
    if (!zoneWaterEnabled) {
        return;
    }
    constexpr auto liveInfoZone = ForwardDataSpace::Zone::ZoneType::LIVE_INFO;
    constexpr auto bitMapZone = ForwardDataSpace::Zone::ZoneType::BIT_MAP;
    for (size_t tag = 0; tag < 2; ++tag) {
        ForwardDataSpace::ZoneWaterSnapshot snapshot = liveInfoData[tag].GetZoneWaterSnapshot(false);
        for (size_t i = liveInfoZone; i < ForwardDataSpace::Zone::ZoneType::TOTAL_NUM; ++i) {
            zoneWaterAllocationCount[i] += snapshot.allocationCount[i];
            if (snapshot.highWater[i] > zoneWaterHighWater[i]) {
                zoneWaterHighWater[i] = snapshot.highWater[i];
            }
        }
    }
    size_t liveInfoQuota = liveInfoData[0].GetZoneCapacity(liveInfoZone);
    size_t bitMapQuota = liveInfoData[0].GetZoneCapacity(bitMapZone);
    size_t tagLen = liveInfoData[0].GetSpaceSize();
    size_t combined = zoneWaterHighWater[liveInfoZone] + zoneWaterHighWater[bitMapZone];
    double liveInfoPercent = liveInfoQuota == 0 ? 0.0 :
        100.0 * static_cast<double>(zoneWaterHighWater[liveInfoZone]) / static_cast<double>(liveInfoQuota);
    double bitMapPercent = bitMapQuota == 0 ? 0.0 :
        100.0 * static_cast<double>(zoneWaterHighWater[bitMapZone]) / static_cast<double>(bitMapQuota);
    unsigned overQuota = (zoneWaterHighWater[liveInfoZone] > liveInfoQuota ||
                          zoneWaterHighWater[bitMapZone] > bitMapQuota || zoneWaterEverOverQuota != 0) ? 1u : 0u;
    unsigned overZone = (combined > tagLen || zoneWaterEverOverZone != 0) ? 1u : 0u;
    std::fprintf(stderr,
        "[ZONEWATER-FINAL] LI_uses=%zu LI_hw=%zu/%zu LI_pct=%.6f%% "
        "BM_uses=%zu BM_hw=%zu/%zu BM_pct=%.6f%% OVER_QUOTA=%u OVER_ZONE=%u UNBIND_OVER=%u\n",
        zoneWaterAllocationCount[liveInfoZone], zoneWaterHighWater[liveInfoZone], liveInfoQuota, liveInfoPercent,
        zoneWaterAllocationCount[bitMapZone], zoneWaterHighWater[bitMapZone], bitMapQuota, bitMapPercent,
        overQuota, overZone, zoneWaterUnbindOver);
    std::fflush(stderr);
}

void ForwardDataManager::ForwardDataSpace::UnbindPreviousLiveInfo()
{
    auto& zone = allocZone[ForwardDataSpace::Zone::ZoneType::LIVE_INFO];
    size_t start = zone.zoneStartAddress;
    size_t pos = zone.zonePosition.load();
    size_t quota = zone.zoneEndAddress - zone.zoneStartAddress;
    size_t used = pos > start ? pos - start : 0;
    unsigned over = used > quota ? 1u : 0u;
    size_t iters = used / sizeof(LiveInfo);
    if (zoneWaterEnabled) {
        std::fprintf(stderr,
            "[ZONEWATER-UNBIND] tag_space=%#zx pos=%zu quota=%zu used=%zu over=%u iters=%zu\n",
            start, pos, quota, used, over, iters);
        if (over != 0) {
            ForwardDataManager& mgr = ForwardDataManager::GetForwardDataManager();
            mgr.zoneWaterUnbindOver = 1;
            size_t printed = 0;
            size_t quotaEnd = start + quota;
            for (size_t current = quotaEnd; current < pos && printed < 3; current += sizeof(LiveInfo), ++printed) {
                LiveInfo* currentLiveInfo = reinterpret_cast<LiveInfo*>(current);
                // Print bindedRegion as numeric value only; do not dereference it.
                std::fprintf(stderr,
                    "[ZONEWATER-UNBIND-EXTRA] idx=%zu addr=%#zx bindedRegion=%zu\n",
                    printed, current, reinterpret_cast<size_t>(currentLiveInfo->bindedRegion));
            }
        }
        std::fflush(stderr);
    }
    for (size_t current = start; current < pos; current += sizeof(LiveInfo)) {
        LiveInfo* currentLiveInfo = reinterpret_cast<LiveInfo*>(current);
        RegionInfo* bindedRegion = currentLiveInfo->bindedRegion;
        CHECK(bindedRegion != nullptr);
        bindedRegion->CheckAndClearLiveInfo(currentLiveInfo);
    }
}
} // namespace MapleRuntime
