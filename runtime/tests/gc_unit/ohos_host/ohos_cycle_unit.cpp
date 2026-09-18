// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/z/zCrossVM.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <list>

#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#include "root_publication_snapshot.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

namespace MapleRuntime {
struct ZGenerationRootTestAccess {
    static void Seed(HeapGcState& collector, BaseObject* object)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.emplace(ValueRoot(object),
                                            ValueRootList{});
    }

    static void Clear(HeapGcState& collector)
    {
        std::lock_guard<std::mutex> lock(Heap::GetHeap().cross_vm().cycleWorkStackMtx);
        Heap::GetHeap().cross_vm().cycleRefWorkStack.clear();
    }
    static void PostResolveCycleTask(HeapGcState& collector) { Heap::GetHeap().cross_vm().PostResolveCycleTask(); }
};
} // namespace MapleRuntime

namespace {
std::atomic<unsigned> gPosted{0};
std::atomic<bool> gMajorRootObserved{false};
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

void* RunMajorCycle(void*)
{
    // ZHeap owns both generations (zHeap.cpp:60-70); a major request runs
    // its young prelude before the old body (zDriver.cpp:443-451). Use the
    // initialized heap collector and driver instead of a second collector.
    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
    alignas(TypeInfo) static unsigned char typeStorage[sizeof(TypeInfo)] {};
    auto* type = reinterpret_cast<TypeInfo*>(typeStorage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uint64_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(typeStorage), sizeof(typeStorage));
    auto* object = MObject::NewObject(type, 16, AllocType::MOVEABLE_OBJECT);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(object);
    ZGenerationRootTestAccess::Seed(collector, object);
    // Read the product's published old mark stacks at the top of DoTracing
    // (after the old root task returned, before follow). The export root
    // keeping the object alive is not in this window (it feeds the driver's
    // foreign stack), so the cycle-owner family scan is what publishes it.
    collector.testOldMarkStarted = [handle, &collector]() {
        BaseObject* current = Heap::GetHeap().GetExportObject(handle);
        const bool found = RootPublicationSnapshot::Contains(*collector.MajorMark(), current);
        gMajorRootObserved.store(found, std::memory_order_relaxed);
        std::printf("OHOS_HOST_ROOT_RESULT current=%p found=%u\n",
                    static_cast<void*>(current), static_cast<unsigned>(found));
        std::fflush(stdout);
    };
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    collector.testOldMarkStarted = nullptr;
    ZGenerationRootTestAccess::Clear(collector);
    Heap::GetHeap().RemoveExportObject(handle);
    return nullptr;
}
} // namespace

GC_TEST(OHOSCycle, PostResolvePostsProductTask)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    HeapGcState& collector = Heap::GetHeap().GetCollector();
    ZGenerationRootTestAccess::Seed(collector, reinterpret_cast<BaseObject*>(uintptr_t{1}));
    ZGenerationRootTestAccess::PostResolveCycleTask(collector);
    ZGenerationRootTestAccess::Clear(collector);
    ExpectPostState("OHOSCycle.PostResolvePostsProductTask", 1U);
}

GC_TEST(OHOSCycle, EmptyWorkDoesNotPost)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    HeapGcState& collector = Heap::GetHeap().GetCollector();
    ZGenerationRootTestAccess::Clear(collector);
    ZGenerationRootTestAccess::PostResolveCycleTask(collector);
    ExpectPostState("OHOSCycle.EmptyWorkDoesNotPost", 0U);
}

GC_TEST(OHOSCycle, MajorEntryPostsResolveTask)
{
    RuntimeParam param {};
    param.heapParam.heapSize = 32 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RegisterEventHandlerCallbacks(&RecordPost, &NoHigherPriorityTask);
    CJThreadHandle handle = RunCJTask(RunMajorCycle, nullptr);
    GC_EXPECT_TRUE(handle != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &result), E_OK);
    ReleaseHandle(handle);
    ExpectPostState("OHOSCycle.MajorEntryPostsResolveTask", 1U);
    GC_EXPECT_TRUE(gMajorRootObserved.load(std::memory_order_relaxed));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
