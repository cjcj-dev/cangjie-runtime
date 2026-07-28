// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "CjScheduler.h"
#include "Heap/Heap.h"
#include "Heap/StickyLog.h"

namespace MapleRuntime {
namespace {
constexpr uint8_t LOGGED_LINE_VALUE = 1;

bool RunReclaimCase(RegionManager& manager, size_t num)
{
    RegionInfo* region = manager.TakeRegion(num, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("STICKY_REGION_CLEAR units=%zu result=FAIL reason=take-region\n", num);
        return false;
    }

    MAddress heapStart = __cj_sticky_heap_base;
    size_t totalLineCount = __cj_sticky_heap_size / StickyLog::LINE_SIZE;
    std::fill_n(__cj_sticky_logged_base, totalLineCount, LOGGED_LINE_VALUE);

    MAddress regionStart = region->GetRegionStart();
    MAddress regionEnd = regionStart + num * RegionInfo::UNIT_SIZE;
    manager.ReclaimRegion(region);

    size_t clearedLineCount = 0;
    size_t changedOutsideCount = 0;
    for (size_t lineIndex = 0; lineIndex < totalLineCount; ++lineIndex) {
        MAddress lineStart = heapStart + lineIndex * StickyLog::LINE_SIZE;
        bool inExpectedInterval = lineStart >= regionStart && lineStart < regionEnd;
        uint8_t value = __cj_sticky_logged_base[lineIndex];
        if (inExpectedInterval && value == 0) {
            ++clearedLineCount;
        } else if (!inExpectedInterval && value != LOGGED_LINE_VALUE) {
            ++changedOutsideCount;
        }
    }

    size_t expectedLineCount = num * RegionInfo::UNIT_SIZE / StickyLog::LINE_SIZE;
    bool passed = clearedLineCount == expectedLineCount && changedOutsideCount == 0;
    std::printf(
        "STICKY_REGION_CLEAR units=%zu interval=[%#zx,%#zx) cleared_lines=%zu expected_lines=%zu "
        "outside_changed=%zu result=%s\n",
        num, regionStart, regionEnd, clearedLineCount, expectedLineCount, changedOutsideCount,
        passed ? "PASS" : "FAIL");
    return passed;
}
} // namespace
} // namespace MapleRuntime

int main()
{
    MapleRuntime::MRT_CjRuntimeInit();
    auto& allocator = reinterpret_cast<MapleRuntime::RegionSpace&>(MapleRuntime::Heap::GetHeap().GetAllocator());
    MapleRuntime::RegionManager& manager = allocator.GetRegionManager();
    bool multiUnitPassed = MapleRuntime::RunReclaimCase(manager, 2);
    bool singleUnitPassed = MapleRuntime::RunReclaimCase(manager, 1);
    MapleRuntime::CangjieRuntime::FiniAndDelete();
    return multiUnitPassed && singleUnitPassed ? 0 : 1;
}
