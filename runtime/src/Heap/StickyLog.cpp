// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StickyLog.h"

#include "Allocator/MemMap.h"
#include "Base/ImmortalWrapper.h"
#include "Base/MemUtils.h"
#include "Base/Panic.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
extern "C" MRT_EXPORT uint8_t* __cj_sticky_logged_base = nullptr;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_base = 0;
extern "C" MRT_EXPORT const uint8_t __cj_sticky_line_shift = StickyLog::LINE_SHIFT;

static ImmortalWrapper<StickyLog> g_stickyLog;

StickyLog& StickyLog::Instance() noexcept { return *g_stickyLog; }

void StickyLog::Init(MAddress start, size_t size)
{
    MRT_ASSERT(loggedMap == nullptr, "sticky logged map initialized twice");
    MRT_ASSERT((start & (LINE_SIZE - 1)) == 0, "heap start is not sticky-line aligned");
    heapStart = start;
    heapSize = size;
    loggedByteCount = (size + LINE_SIZE - 1) >> LINE_SHIFT;

    MemMap::Option option = MemMap::DEFAULT_OPTIONS;
    option.tag = "cangjie_sticky_logged";
    loggedMap = MemMap::MapMemory(loggedByteCount, loggedByteCount, option);
#ifdef _WIN64
    MemMap::CommitMemory(loggedMap->GetBaseAddr(), loggedByteCount);
#endif
    __cj_sticky_logged_base = reinterpret_cast<uint8_t*>(loggedMap->GetBaseAddr());
    __cj_sticky_heap_base = heapStart;
}

void StickyLog::Fini() noexcept
{
    __cj_sticky_logged_base = nullptr;
    __cj_sticky_heap_base = 0;
    MemMap::DestroyMemMap(loggedMap);
    heapStart = 0;
    heapSize = 0;
    loggedByteCount = 0;
    enabled = false;
}

bool StickyLog::IsLoggedLine(MAddress address) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return false;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    return *reinterpret_cast<volatile uint8_t*>(__cj_sticky_logged_base + lineIndex) != 0;
}

bool StickyLog::TryLogLine(MAddress address, MAddress& lineStart) const
{
    if (UNLIKELY(address < heapStart || address >= heapStart + heapSize || __cj_sticky_logged_base == nullptr)) {
        return false;
    }
    size_t lineIndex = (address - heapStart) >> LINE_SHIFT;
    volatile uint8_t* loggedByte = __cj_sticky_logged_base + lineIndex;
    if (*loggedByte != 0) {
        return false;
    }
    *loggedByte = 1;
    lineStart = heapStart + (lineIndex << LINE_SHIFT);
    return true;
}

void StickyLog::ClearUnavailableRegion(MAddress regionStart, size_t regionSize)
{
    MRT_ASSERT(regionStart >= heapStart && regionStart + regionSize <= heapStart + heapSize,
               "sticky region clear is outside heap");
    MRT_ASSERT((regionStart & (LINE_SIZE - 1)) == 0 && (regionSize & (LINE_SIZE - 1)) == 0,
               "sticky region clear is not line aligned");
    size_t firstLine = (regionStart - heapStart) >> LINE_SHIFT;
    size_t lineCount = regionSize >> LINE_SHIFT;
    MemorySet(reinterpret_cast<uintptr_t>(__cj_sticky_logged_base + firstLine), lineCount, 0, lineCount);
}

void StickyLog::BeginEpoch()
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "sticky epoch may only advance while mutators are stopped");
    MemorySet(reinterpret_cast<uintptr_t>(__cj_sticky_logged_base), loggedByteCount, 0, loggedByteCount);
}

extern "C" MRT_EXPORT void CJ_MCC_StickyLogLine(BaseObject* object)
{
    if (object == nullptr) {
        return;
    }
    StickyLog& stickyLog = StickyLog::Instance();
    MAddress address = reinterpret_cast<MAddress>(object);
    if (LIKELY(stickyLog.IsLoggedLine(address))) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (UNLIKELY(mutator == nullptr)) {
        return;
    }
    MAddress lineStart = 0;
    if (stickyLog.TryLogLine(address, lineStart)) {
        mutator->RememberLineInStickyLogBuffer(lineStart);
    }
}
} // namespace MapleRuntime
