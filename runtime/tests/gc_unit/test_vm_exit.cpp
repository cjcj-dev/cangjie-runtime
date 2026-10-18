// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SafepointSynchronize.h"
#include "Mutator/VMExit.h"
#include "Mutator/VMOperation.h"
#include <cstdio>
#include <thread>

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

#if defined(__linux__) && !defined(__OHOS__)
GC_RUNTIME_OTHER_VM_TEST(VMService1350, PublicHostHeapDumpAttachesAndDetachesOnEveryCall)
{
    auto params = ShutdownParameters();
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const auto mainHandle = MutatorManager::Instance().GetMainThreadHandle();
    bool detached[2] = {false, false};
    bool written[2] = {false, false};
    std::thread host([&] {
        for (int i = 0; i != 2; ++i) {
            FILE* output = std::tmpfile();
            if (output == nullptr) { return; }
            CJ_MRT_ForceFullGC();
            CJ_MRT_DumpHeapSnapshot(fileno(output));
            detached[i] = Mutator::GetMutator() == nullptr;
            written[i] = std::fseek(output, 0, SEEK_END) == 0 && std::ftell(output) > 0;
            std::fclose(output);
        }
    });
    host.join();
    GC_EXPECT_TRUE(detached[0] && detached[1]);
    GC_EXPECT_TRUE(written[0] && written[1]);
    GC_EXPECT_TRUE(MutatorManager::Instance().GetMainThreadHandle() == mainHandle);
    std::fprintf(stderr, "VM1350_PUBLIC_HOST_TARGET executed=1 calls=2 detached=2 output=2 main_preserved=1\n");
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif
