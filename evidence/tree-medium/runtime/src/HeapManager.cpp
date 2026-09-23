// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "HeapManager.h"

#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zArguments.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "CangjieRuntime.h"

namespace MapleRuntime {
HeapManager::HeapManager() {}

MAddress HeapManager::Allocate(size_t allocSize, AllocType allocType)
{
    return Heap::GetHeap().Allocate(allocSize, allocType);
}

void HeapManager::Init(const HeapParam& param)
{
    // Heap sizing precedes collector construction and runtime worker selection
    // (Universe::initialize_heap_sizes -> create_heap, zArguments.cpp:243).
    ZHeuristics::set_max_heap_size(param.heapSize * 1024);
    ZArguments::initialize();
    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    ZCollectedHeap::create(param, CangjieRuntime::GetGCParam().garbageThreshold);
    Heap::GetHeap().Init();
}

void HeapManager::Fini() { Heap::GetHeap().Fini(); }
} // namespace MapleRuntime
