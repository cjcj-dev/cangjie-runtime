// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>

#include "Cangjie.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace {
struct RegionFixture {
    RegionFixture()
    {
        mappedSize = sizeof(RegionInfo) + RegionInfo::UNIT_SIZE;
        mapping = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            std::abort();
        }
        heapStart = reinterpret_cast<MAddress>(mapping) + sizeof(RegionInfo);
        RegionInfo::Initialize(1, heapStart);
        region = RegionInfo::InitRegion(0, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        object = reinterpret_cast<BaseObject*>(heapStart + 64);
        region->SetRegionAllocPtr(reinterpret_cast<MAddress>(object) + 64);
    }

    ~RegionFixture() { munmap(mapping, mappedSize); }

    void* mapping = nullptr;
    size_t mappedSize = 0;
    MAddress heapStart = 0;
    RegionInfo* region = nullptr;
    BaseObject* object = nullptr;
};

size_t InvalidSizeFor(const std::string& testCase)
{
    if (testCase == "zero") {
        return 0;
    }
    if (testCase == "unaligned") {
        return 10;
    }
    if (testCase == "oversize") {
        return RegionInfo::UNIT_SIZE;
    }
    std::cerr << "unknown case: " << testCase << '\n';
    std::exit(2);
}
} // namespace
} // namespace MapleRuntime

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: object_size_guard_harness zero|unaligned|oversize\n";
        return 2;
    }
    RuntimeParam runtimeParam {};
    runtimeParam.heapParam.heapSize = 4 * 1024;
    runtimeParam.coParam.processorNum = 1;
    if (InitCJRuntime(&runtimeParam) != E_OK) {
        std::cerr << "failed to initialize runtime\n";
        return 2;
    }
    MapleRuntime::RegionFixture fixture;
    size_t invalidSize = MapleRuntime::InvalidSizeFor(argv[1]);
    std::cerr << "GUARD_FIRES_PROOF invoking case=" << argv[1] << " obj=" << fixture.object
              << " objSize=" << invalidSize << '\n';
    (void)fixture.region->MarkObject(
        fixture.region->GetMarkView<Generation::Old>(), fixture.object, invalidSize);
    std::cerr << "GUARD_DID_NOT_FIRE\n";
    return 1;
}
