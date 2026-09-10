// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <list>

#include "Cangjie.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

namespace {
std::atomic<unsigned> gPosted{0};
void* gTask = nullptr;

bool RecordPost(void* task)
{
    gTask = task;
    gPosted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool NoHigherPriorityTask()
{
    return false;
}

class PostResolveProbeCollector final : public WCollector {
public:
    using WCollector::DoGarbageCollection;

    PostResolveProbeCollector(Allocator& allocator, CollectorResources& resources)
        : WCollector(allocator, resources)
    {
    }

    void SeedCycleWork()
    {
        cycleRefWorkStack.emplace(reinterpret_cast<BaseObject*>(uintptr_t{1}), std::list<BaseObject*>{});
    }
};

void ExpectPostState(const char* test, unsigned expected)
{
    const unsigned posted = gPosted.load(std::memory_order_relaxed);
    const unsigned taskNonNull = gTask != nullptr ? 1U : 0U;
    std::printf("OHOS_HOST_ASSERT_REACHED test=%s posted=%u task_nonnull=%u expected=%u\n",
                test, posted, taskNonNull, expected);
    std::fflush(stdout);
    GC_EXPECT_EQ(posted, expected);
    GC_EXPECT_EQ(taskNonNull, expected);
}
} // namespace

GC_TEST(OHOSCycle, PostResolvePostsProductTask)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    PostResolveProbeCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SeedCycleWork();
    collector.PostResolveCycleTask();
    ExpectPostState("OHOSCycle.PostResolvePostsProductTask", 1U);
}

GC_TEST(OHOSCycle, EmptyWorkDoesNotPost)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    PostResolveProbeCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.PostResolveCycleTask();
    ExpectPostState("OHOSCycle.EmptyWorkDoesNotPost", 0U);
}

GC_TEST(OHOSCycle, MajorEntryPostsResolveTask)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    PostResolveProbeCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    collector.SeedCycleWork();
    collector.DoGarbageCollection();
    ExpectPostState("OHOSCycle.MajorEntryPostsResolveTask", 1U);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
