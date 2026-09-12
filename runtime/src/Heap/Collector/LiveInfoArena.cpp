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
#include "Heap/Heap.h"
#include "LiveInfo.h"
#include "LiveInfoArena.h"

namespace MapleRuntime {

static ImmortalWrapper<LiveInfoArena> liveInfoArena;
LiveInfoArena& LiveInfoArena::GetLiveInfoArena() { return *liveInfoArena; }

void LiveInfoArena::InitializeForwardData()
{
    size_t maxHeapBytes = Heap::GetHeap().GetMaxCapacity();
    size_t liveInfoSize = RoundUp(GetLiveInfoDataSize(maxHeapBytes), MapleRuntime::MRT_PAGE_SIZE);
    forwardDataSize = liveInfoSize;

#ifdef _WIN64
    void* startAddress = VirtualAlloc(NULL, forwardDataSize, MEM_RESERVE, PAGE_READWRITE);
    if (startAddress == NULL) {
        LOG(RTLOG_FATAL, "failed to initialize live-info arena");
    }
#else
    void* startAddress = mmap(nullptr, forwardDataSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (startAddress == MAP_FAILED) {
        LOG(RTLOG_FATAL, "failed to initialize live-info arena");
    } else {
#ifndef __APPLE__
        (void)madvise(startAddress, forwardDataSize, MADV_NOHUGEPAGE);
        MRT_PRCTL(startAddress, forwardDataSize, "forward_data");
#endif
    }
#endif

    forwardDataStart = reinterpret_cast<uintptr_t>(startAddress);
    liveInfoData.InitializeMemory(forwardDataStart, liveInfoSize, regionUnitCount);
}
} // namespace MapleRuntime
