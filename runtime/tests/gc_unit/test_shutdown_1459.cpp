#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include <atomic>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
void InitializeShutdownRuntime()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 2;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 1;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 1;
    params.gcParam.oldGCThreadsSet = true;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
}

void FinishShutdownRuntime()
{
    std::fprintf(stderr, "SHUTDOWN1459_FINI_BEGIN\n");
    auto rc = FiniCJRuntime();
    bool deleted = MapleRuntime::Runtime::CurrentRef() == nullptr;
    std::fprintf(stderr, "SHUTDOWN1459_TARGET fini_rc=%d runtime_deleted=%d\n", rc, deleted);
    GC_EXPECT_EQ(rc, E_OK);
    GC_EXPECT_TRUE(deleted);
}
}

GC_RUNTIME_OTHER_VM_TEST(Shutdown1459, BootstrapReturnsBeforeRuntimeDelete)
{
    InitializeShutdownRuntime();
    FinishShutdownRuntime();
}

GC_RUNTIME_OTHER_VM_TEST(Shutdown1459, CompletedCarriersReturnBeforeRuntimeDelete)
{
    InitializeShutdownRuntime();
    std::atomic<long> carrier{0};
    auto task = RunCJTask([](void* input) -> void* {
        static_cast<std::atomic<long>*>(input)->store(syscall(SYS_gettid));
        return nullptr;
    }, &carrier);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "SHUTDOWN1459_CARRIER tid=%ld\n", carrier.load());
    GC_EXPECT_TRUE(carrier.load() > 0);
    FinishShutdownRuntime();
}
