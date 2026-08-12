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

#include "Base/ImmortalWrapper.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionManager.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "Heap/Verify/TagReuseProbe.h"
#include "LiveInfo.h"
#include "ForwardDataManager.h"

namespace MapleRuntime {

static ImmortalWrapper<ForwardDataManager> forwardDataManager;
ForwardDataManager& ForwardDataManager::GetForwardDataManager() { return *forwardDataManager; }

void ForwardDataManager::ClearPreviousForwardData()
{
    uint16_t prev = GetPreviousTagID();
    ForwardDataSpace& space = liveInfoData[prev];
    uintptr_t rangeStart = space.GetStartAddress();
    size_t rangeSize = space.GetSize();
    uintptr_t liveStart = space.GetZoneStart(ForwardDataSpace::Zone::ZoneType::LIVE_INFO);
    uintptr_t livePos = space.GetZonePos(ForwardDataSpace::Zone::ZoneType::LIVE_INFO);
    uintptr_t bmStart = space.GetZoneStart(ForwardDataSpace::Zone::ZoneType::BIT_MAP);
    uintptr_t bmPos = space.GetZonePos(ForwardDataSpace::Zone::ZoneType::BIT_MAP);
    // Structural guarantee: no region field may still address the range about to be madvise'd.
    // Order forced here (not by convention): null → probe (optional) → ReleaseMemory.
    RegionSpace& regionSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    regionSpace.GetRegionManager().NullLiveInfoFieldsInRange(rangeStart, rangeSize);
    TagReuseProbe::ScanBeforeRelease(rangeStart, rangeSize, prev, liveStart, livePos, bmStart, bmPos);
    space.ReleaseMemory();
}

void ForwardDataManager::InitializeForwardData()
{
    size_t maxHeapBytes = Heap::GetHeap().GetMaxCapacity();
    size_t liveInfoSize = RoundUp(GetLiveInfoDataSize(maxHeapBytes), MapleRuntime::MRT_PAGE_SIZE);
    // One liveInfo slot per tag generation (TAG_ID_COUNT).
    forwardDataSize = liveInfoSize * TAG_ID_COUNT;

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
    for (uint16_t i = 0; i < TAG_ID_COUNT; ++i) {
        liveInfoData[i].InitializeMemory(forwardDataStart + static_cast<size_t>(i) * liveInfoSize, liveInfoSize,
                                         regionUnitCount);
    }
}
void ForwardDataManager::ForwardDataSpace::UnbindPreviousLiveInfo()
{
    auto& zone = allocZone[ForwardDataSpace::Zone::ZoneType::LIVE_INFO];
    size_t start = zone.zoneStartAddress;
    size_t pos = zone.zonePosition.load();
    for (size_t current = start; current < pos; current += sizeof(LiveInfo)) {
        LiveInfo* currentLiveInfo = reinterpret_cast<LiveInfo*>(current);
        RegionInfo* bindedRegion = currentLiveInfo->bindedRegion;
        CHECK(bindedRegion != nullptr);
        bindedRegion->CheckAndClearLiveInfo(currentLiveInfo);
    }
}
} // namespace MapleRuntime
