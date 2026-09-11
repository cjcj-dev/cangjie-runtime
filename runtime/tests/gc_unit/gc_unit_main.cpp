// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gc_unittest.hpp"

// Standalone unit processes do not run Heap::Init(), while several tests enter
// product barriers or GC request APIs that dereference CollectorProxy's active
// collector. A whole-suite process used to inherit this binding from an earlier
// test, which made those cases order-dependent. Bind the proxy's built-in
// collector at the isolated-process entry without initializing or running it.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
struct RelocationReceiptTestAccess {
    static void PrepareIsolatedGcUnit()
    {
        CollectorProxy& proxy = Heap::GetHeap().GetCollectorResources().collectorProxy;
        if (proxy.currentCollector == nullptr) {
            proxy.currentCollector = &proxy.wCollector;
        }
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
