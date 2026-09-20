// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Mutator/ThreadLocal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gc_unittest.hpp"

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zCPU.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeuristics.hpp"

void MapleRuntime::GcUnit::CreateStandaloneHeap(size_t units)
{
    if (Heap::heap() == nullptr) {
        HeapParam params{};
        params.heapSize = units * ZGranuleSize / 1024;
        params.regionSize = ZGranuleSize / 1024;
        params.exemptionThreshold = 0.8;
        ZHeuristics::set_max_heap_size(params.heapSize * 1024);
        ZCollectedHeap::create(params, 0.5);
    }
}

int main(int argc, char** argv)
{
    // Standalone fixtures create worker pools without GCThread::Init. Set the
    // maximum before any ZPerWorker storage; logical active counts may vary.
    MapleRuntime::ConcGCThreads = 64;
    MapleRuntime::ZGlobalsPointers::initialize();
    MapleRuntime::ThreadLocal::InitializeCleaner();
    // zInitialize.cpp:62: the CPU affinity table precedes any ZCPU::id() reader.
    MapleRuntime::ZCPU::initialize();
    constexpr const char* filterPrefix = "--gtest_filter=";
    constexpr const char* listTests = "--gtest_list_tests";
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [--gtest_list_tests|--gtest_filter=Suite.Test]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], listTests) == 0) {
            (void)setenv("GC_UNIT_LIST_TESTS", "1", 1);
            continue;
        }
        if (std::strncmp(argv[i], filterPrefix, std::strlen(filterPrefix)) != 0 ||
            argv[i][std::strlen(filterPrefix)] == '\0') {
            std::fprintf(stderr, "usage: %s [--gtest_list_tests|--gtest_filter=Suite.Test]\n", argv[0]);
            return 2;
        }
        (void)setenv("GC_UNIT_FILTER", argv[i] + std::strlen(filterPrefix), 1);
    }
    MapleRuntime::GcUnit::InitializeStandaloneHeap = [] { MapleRuntime::GcUnit::CreateStandaloneHeap(64); };
    const int result = MapleRuntime::GcUnit::RunAll();
    // Stop only an existing heap; listing/filtering must not construct one.
    if (MapleRuntime::Heap::heap() != nullptr) {
        MapleRuntime::Heap::GetHeap().StopGCWork();
    }
    return result;
}
