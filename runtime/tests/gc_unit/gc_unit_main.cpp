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

// Standalone unit processes obtain the unique product heap collector before
// entering tests, without starting a runtime or a collection.
// Parse value-owned heap resources with the product macro configuration before
// enabling the existing test peers; their member offsets must match the SO.
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"

#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zCPU.hpp"

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void PrepareIsolatedGcUnit()
    {
        (void)Heap::GetHeap().GetCollector();
    }
};
} // namespace MapleRuntime

namespace {
void PrepareIsolatedGcUnitProcess()
{
    MapleRuntime::RelocationReceiptTestAccess::PrepareIsolatedGcUnit();
}
} // namespace

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
    bool isolatedTest = false;
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
        isolatedTest = true;
    }
    if (isolatedTest) {
        PrepareIsolatedGcUnitProcess();
    }
    return MapleRuntime::GcUnit::RunAll();
}
