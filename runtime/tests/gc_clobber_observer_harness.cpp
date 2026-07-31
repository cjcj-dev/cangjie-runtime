// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "CangjieRuntime.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace {
volatile uint8_t observerTarget = 0x5a;

__attribute__((noinline)) uint8_t DereferenceObservedWord(uint64_t observed)
{
    return *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(observed));
}
} // namespace
} // namespace MapleRuntime

int main()
{
    MapleRuntime::MRT_CjRuntimeInit();
    auto& allocator =
        reinterpret_cast<MapleRuntime::RegionSpace&>(MapleRuntime::Heap::GetHeap().GetAllocator());
    MapleRuntime::RegionManager& manager = allocator.GetRegionManager();
    MapleRuntime::RegionInfo* region =
        manager.TakeRegion(1, MapleRuntime::RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    if (region == nullptr) {
        std::printf("GC_CLOBBER_OBSERVER result=FAIL reason=take-region\n");
        std::fflush(stdout);
        std::_Exit(2);
    }

    uintptr_t regionStart = region->GetRegionStart();
    *reinterpret_cast<volatile uintptr_t*>(regionStart) =
        reinterpret_cast<uintptr_t>(&MapleRuntime::observerTarget);
    manager.ReclaimRegion(region);

    uint64_t observed = *reinterpret_cast<volatile uint64_t*>(regionStart);
    std::printf("GC_CLOBBER_OBSERVER observed=%#018" PRIx64 " width=64\n", observed);
    std::fflush(stdout);
    uint8_t value = MapleRuntime::DereferenceObservedWord(observed);
    std::printf("GC_CLOBBER_OBSERVER dereference=PASS value=%#04x\n", value);
    std::fflush(stdout);
    std::_Exit(value == MapleRuntime::observerTarget ? 0 : 3);
}
