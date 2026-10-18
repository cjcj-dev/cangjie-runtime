// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Mutator/Mutator.h"
#include "Mutator/SafepointSynchronize.h"
#include "Mutator/VMExit.h"
#include "Mutator/VMOperation.h"
#include <cstdio>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
RuntimeParam ShutdownParameters()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 2;
    params.gcParam.staticGCThreads = true;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 2;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 2;
    params.gcParam.oldGCThreadsSet = true;
    return params;
}
struct CallerObservation {
    int result = E_OK;
    bool identityPreserved = false;
    bool closing = true;
};
void* InvalidShutdownFromTask(void* opaque)
{
    auto& result = *static_cast<CallerObservation*>(opaque);
    Mutator* before = Mutator::GetMutator();
    result.result = FiniCJRuntime();
    result.identityPreserved = before != nullptr && Mutator::GetMutator() == before && !before->IsGCDetached();
    result.closing = VM_Exit::closing();
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(VMExit1473, UninitializedShutdownDoesNotPublishClosing)
{
    GC_EXPECT_EQ(FiniCJRuntime(), E_FAILED);
    GC_EXPECT_FALSE(VM_Exit::closing());
    GC_EXPECT_FALSE(VM_Exit::exited());
    std::fprintf(stderr, "VM1473_UNINITIALIZED_TARGET executed=1 rejected=1 unpublished=1\n");
}

GC_RUNTIME_OTHER_VM_TEST(VMExit1473, SchedulerCallerIsRejectedBeforeDetach)
{
    auto params = ShutdownParameters();
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    CallerObservation observation;
    auto task = RunCJTask(InvalidShutdownFromTask, &observation);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    GC_EXPECT_EQ(observation.result, E_FAILED);
    GC_EXPECT_TRUE(observation.identityPreserved);
    GC_EXPECT_FALSE(observation.closing);
    std::fprintf(stderr, "VM1473_QUALIFICATION_TARGET executed=1 identity_preserved=1 unpublished=1\n");
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(VMExit1473, HostSuccessRequiresFinalStateAndRepeatFails)
{
    auto params = ShutdownParameters();
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    GC_EXPECT_TRUE(VM_Exit::exited());
    GC_EXPECT_TRUE(VMThread::is_terminated());
    GC_EXPECT_TRUE(SafepointSynchronize::is_at_safepoint());
    GC_EXPECT_TRUE(SafepointSynchronize::is_synchronizing());
    GC_EXPECT_TRUE(Runtime::CurrentRef() == nullptr);
    GC_EXPECT_EQ(FiniCJRuntime(), E_FAILED);
    std::fprintf(stderr, "VM1473_HOST_TARGET executed=1 terminal=1 final_pause=1 facade_deleted=1 repeat_rejected=1\n");
}
