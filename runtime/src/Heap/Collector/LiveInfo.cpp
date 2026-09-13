// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/ImmortalWrapper.h"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "LiveInfoArena.h"
#include "LiveInfo.h"

namespace MapleRuntime {
uint64_t RegionInfo::GetSnapshotEpoch() const
{
    const GCCycleGeneration generation = GetOwnerGeneration() == Generation::Young
        ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
    return Heap::GetHeap().GetCollector().GetCycleSnapshot(generation).sequence;
}
} // namespace MapleRuntime
