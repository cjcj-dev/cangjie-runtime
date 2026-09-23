// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// zStat.cpp:65-240,386-517,1036-1049. The reference tree has no dedicated
// ZStat gtest; these retain coverage of the public registry and STW entry.
// Private sampler/history construction tests retire with the TU-private types
// per A12b advisor 20260913T211044Z.
#include "gc_unittest.hpp"
#include "CjScheduler.h"
extern "C" int CJ_ScheduleManagerInit();
#include "Heap/z/zStat.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Mutator/ThreadLocal.h"
#include <cstring>
#include <chrono>
#include <thread>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
const ZStatSampler unobserved("Test", "Unobserved", ZStatUnitTimeNs);

// Exercise the product page-allocation entry and read both product rate
// consumers. Each case has a fresh process so sampler history is independent.
void CheckPageAllocationRate(bool relocation, bool initialized)
{
    if (initialized) {
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        MRT_CjRuntimeInit();
    } else {
        CreateStandaloneHeap(64);
    }
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    ZStatMutatorAllocRate::initialize();
    (void)ZStatMutatorAllocRate::counter().GetAndReset();
    const auto before = ZStatMutatorAllocRate::stats();
    ZAllocationFlags flags;
    flags.set_non_blocking();
    if (relocation) flags.set_gc_relocation();
    // Exceed the sampling granule even with the runner's 1024-granule heap.
    const size_t allocationSize = 16 * ZGranuleSize;
    ZPage* page = Heap::alloc_page(allocationSize, ZPageType::large,
                                 false, false, PageAge::eden, flags);
    GC_EXPECT_TRUE(page != nullptr);
    const auto bytes = ZStatMutatorAllocRate::counter().GetAndReset().counter;
    const auto after = ZStatMutatorAllocRate::stats();
    std::fprintf(stderr, "ALLOC_RATE_TARGET initialized=%d relocation=%d bytes=%llu avg_before=%g avg_after=%g\n",
                 initialized, relocation, static_cast<unsigned long long>(bytes), before.avg, after.avg);
    const bool excluded = relocation || !initialized;
    const bool counterCorrect = bytes == (excluded ? 0U : allocationSize);
    const bool samplerCorrect = excluded
        ? after.avg == before.avg && after.predict == before.predict && after.sd == before.sd
        : after.avg > before.avg;
    // Evaluate both consumers before asserting, so neither masks the other.
    GC_EXPECT_TRUE(counterCorrect && samplerCorrect);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationRate855, MutatorPageContributes)
{
    CheckPageAllocationRate(false, true);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationRate855, RelocationPageExcluded)
{
    CheckPageAllocationRate(true, true);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationRate855, PreInitMutatorPageExcluded)
{
    CheckPageAllocationRate(false, false);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationRate855, PreInitRelocationPageExcluded)
{
    CheckPageAllocationRate(true, false);
}

#if defined(__linux__)
// Read the existing product history printer, without exposing TU-private
// sampler data. The outer OTHER_VM supplies a fresh process; its child is
// forked before runtime initialization and contains all GC threads.
GC_RUNTIME_OTHER_VM_TEST(ZStat, ProductPhaseSamplesReachStatistics)
{
    FILE* transcript = std::tmpfile();
    GC_EXPECT_TRUE(transcript != nullptr);
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        alarm(35);
        if (dup2(fileno(transcript), STDOUT_FILENO) < 0 ||
            dup2(fileno(transcript), STDERR_FILENO) < 0) _exit(126);
        RuntimeParam params{};
        params.heapParam.heapSize = 64 * 1024;
        params.coParam.processorNum = 1;
        params.logParam.logLevel = RTLOG_INFO;
        if (InitCJRuntime(&params) != E_OK) _exit(125);
        // The product prints at ten-second intervals. Observe an initial
        // history, then a synchronous, real major collection and its history.
        std::this_thread::sleep_for(std::chrono::seconds(11));
        Heap::GetHeap().RequestGC(GC_REASON_USER, false);
        std::this_thread::sleep_for(std::chrono::seconds(11));
        std::fflush(nullptr);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::rewind(transcript);
    char line[2048];
    bool before = false;
    bool changed = false;
    while (std::fgets(line, sizeof(line), transcript) != nullptr) {
        const char* record = std::strstr(line, "Major Collection: Major Collection ");
        if (record == nullptr) continue;
        unsigned long long values[8]{};
        const int fields = std::sscanf(record,
            "Major Collection: Major Collection %llu/%llu %llu/%llu %llu/%llu %llu/%llu ns",
            &values[0], &values[1], &values[2], &values[3],
            &values[4], &values[5], &values[6], &values[7]);
        std::fprintf(stderr, "ZSTAT_PRODUCT_RECORD %s", record);
        if (fields == 8) {
            before |= values[6] == 0 && values[7] == 0;
            changed |= before && values[6] > 0 && values[7] >= values[6];
        }
    }
    std::fclose(transcript);
    const bool returned = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "ZSTAT_SAMPLE_TARGET executed=1 status=%d before=%d changed=%d returned=%d\n",
                 status, before, changed, returned);
    // Child failures are observations; this is the sampler-result assertion.
    GC_EXPECT_TRUE(before && changed && returned);
}

// ZStatIterableValue::sort rewrites registration links during initialization.
// Keep the parent assertion observable if a const registry node is read-only.
GC_TEST(ZStat, RegistryInitializationWritesConstLinks)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        ZStat::Initialize();
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool initialized = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "ZSTAT_INIT_TARGET executed=1 status=%d initialized=%d\n", status, initialized);
    GC_EXPECT_TRUE(initialized);
}
#endif

GC_TEST(ZStat, RegistrySortedWithoutChangingIdentity)
{
    const auto id = unobserved.Id();
    ZStat::Initialize();
    bool sorted = true;
    size_t comparisons = 0;
    for (const auto* value = ZStatSampler::First(); value != nullptr && value->Next() != nullptr;
         value = value->Next()) {
        const int group = std::strcmp(value->Group(), value->Next()->Group());
        sorted &= group < 0 || (group == 0 && std::strcmp(value->Name(), value->Next()->Name()) <= 0);
        ++comparisons;
    }
    std::fprintf(stderr, "ZSTAT_SORT_ASSERT_EXECUTED comparisons=%zu sorted=%d\n", comparisons, sorted);
    GC_EXPECT_TRUE(comparisons > 0);
    GC_EXPECT_TRUE(sorted);
    GC_EXPECT_EQ(unobserved.Id(), id);
}

GC_TEST(ZStat, RegistryExistsBeforeSampling)
{
    ZStat::Initialize();
    bool found = false;
    for (const auto* value = ZStatSampler::First(); value != nullptr; value = value->Next()) {
        if (value == &unobserved) found = true;
    }
    GC_EXPECT_TRUE(found);

}


} // namespace
