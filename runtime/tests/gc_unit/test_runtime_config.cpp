// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "CangjieRuntime.h"
#include "RuntimeConfig.h"
#include <cstdlib>
#include <cstdio>

extern "C" void CJ_MRT_CjRuntimeInit();
extern "C" void CJ_MRT_CjRuntimeInitWithConfigV1(const MapleRuntime::RuntimeConfigEntryV1*, size_t);
extern "C" int CJ_ScheduleManagerInit();

namespace {
void CheckHeap(bool embedded, const char* environment, size_t expected)
{
    using namespace MapleRuntime;
    if (environment == nullptr) {
        unsetenv("cjHeapSize");
    } else {
        setenv("cjHeapSize", environment, 1);
    }
    // Keep the configuration alive for the runtime's lifetime, as codegen does.
    static const RuntimeConfigEntryV1 entries[] = {{"cjHeapSize", "64MB"}};
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (embedded) {
        CJ_MRT_CjRuntimeInitWithConfigV1(entries, 1);
    } else {
        CJ_MRT_CjRuntimeInit();
    }
    const size_t actual = CangjieRuntime::GetHeapParam().heapSize;
    std::printf("RUNTIME_CONFIG_HEAP actual_kib=%zu expected_kib=%zu\n", actual, expected);
    std::fflush(stdout);
    GC_EXPECT_EQ(actual, expected);
}
}

// Startup argument delivery corresponds to HotSpot threads.cpp:495, before
// heap initialization. Each case executes the real exported product entry.
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmbeddedHeap) { CheckHeap(true, nullptr, 64 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentHeap) { CheckHeap(false, "64MB", 64 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentOverridesEmbedded) { CheckHeap(true, "128MB", 128 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmptyTableEnvironment)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", "64MB", 1);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    CJ_MRT_CjRuntimeInitWithConfigV1(nullptr, 0);
    const size_t actual = CangjieRuntime::GetHeapParam().heapSize;
    std::printf("RUNTIME_CONFIG_HEAP actual_kib=%zu expected_kib=65536\n", actual);
    GC_EXPECT_EQ(actual, 64 * 1024UL);
}
