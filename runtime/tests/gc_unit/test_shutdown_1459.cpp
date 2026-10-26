#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <thread>
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

#if defined(__linux__) && defined(__x86_64__)
// Invoke the product native-transition ABI; this fixture never invokes hooks.
// C2NStub.S:212-232 consumes its two stack metadata words before returning.
extern "C" void* Shutdown1459CallNative(void* arg, void* (*native)(void*));
asm(".text\n"
    ".globl Shutdown1459CallNative\n"
    ".type Shutdown1459CallNative,@function\n"
    "Shutdown1459CallNative:\n"
    "subq $8, %rsp\n"
    "pushq %rsi\n"
    "pushq $0\n"
    "call CJ_MCC_C2NStub@PLT\n"
    "addq $8, %rsp\n"
    "ret\n"
    ".size Shutdown1459CallNative,.-Shutdown1459CallNative\n");
extern "C" int CJ_SyscallRead(int descriptor, void* buffer, size_t count);
std::atomic<long> shutdown1459BlockedTid{0};
namespace {
std::promise<void>* futureNativeEntered = nullptr;
std::promise<void>* futureNativeReturned = nullptr;
struct NativeReadInput { int descriptor; };
void* NativeRead(void* arg)
{
    shutdown1459BlockedTid.store(syscall(SYS_gettid));
    if (futureNativeEntered != nullptr) { futureNativeEntered->set_value(); }
    char byte = 0;
    int rc = CJ_SyscallRead(static_cast<NativeReadInput*>(arg)->descriptor, &byte, 1);
    return reinterpret_cast<void*>(rc == 1 && byte == 'x' ? 0 : 1);
}
void* NativeReadTask(void* arg)
{
    return Shutdown1459CallNative(arg, NativeRead);
}

std::atomic<bool> futureCallbackReturned{false};
void* NativeReadValueTask(void* arg)
{
    void* failure = Shutdown1459CallNative(arg, NativeRead);
    futureCallbackReturned.store(true, std::memory_order_release);
    if (futureNativeReturned != nullptr) { futureNativeReturned->set_value(); }
    return failure == nullptr ? arg : nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(RuntimeFuture, WaitingGetPublishesCallbackResult)
{
    InitializeShutdownRuntime();
    int descriptors[2];
    GC_EXPECT_EQ(pipe(descriptors), 0);
    NativeReadInput input{descriptors[0]};
    std::promise<void> nativeEntered;
    auto entry = nativeEntered.get_future();
    futureNativeEntered = &nativeEntered;
    shutdown1459BlockedTid.store(0);
    futureCallbackReturned.store(false);
    auto task = RunCJTask(NativeReadValueTask, &input);
    GC_EXPECT_TRUE(task != nullptr);
    const bool entered = entry.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    std::promise<void> started;
    std::promise<int> finished;
    auto ready = started.get_future();
    auto resultReady = finished.get_future();
    void* result = nullptr;
    std::thread waiter([&] {
        started.set_value();
        finished.set_value(GetTaskRet(task, &result));
    });
    ready.wait();
    const bool blocked = resultReady.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
    // Release the actual C2N read even if a predicate failed, so cleanup never
    // requires a second release of the future or a forced runtime shutdown.
    const int written = write(descriptors[1], "x", 1);
    waiter.join();
    const int getRC = resultReady.get();
    const bool returned = futureCallbackReturned.load(std::memory_order_acquire);
    ReleaseHandle(task);
    close(descriptors[0]);
    close(descriptors[1]);
    std::fprintf(stderr, "FUTURE_WAIT_TARGET entered=%d blocked=%d get_rc=%d value=%d callback_returned=%d\n",
                 entered, blocked, getRC, result == &input, returned);
    GC_EXPECT_TRUE(entered && blocked && written == 1 && returned);
    GC_EXPECT_EQ(getRC, E_OK);
    GC_EXPECT_TRUE(result == &input);
    FinishShutdownRuntime();
}

GC_RUNTIME_OTHER_VM_TEST(RuntimeFuture, WaitingReleaseAllowsCallbackCompletion)
{
    InitializeShutdownRuntime();
    int descriptors[2];
    GC_EXPECT_EQ(pipe(descriptors), 0);
    NativeReadInput input{descriptors[0]};
    std::promise<void> nativeEntered;
    std::promise<void> nativeReturned;
    auto entry = nativeEntered.get_future();
    auto completion = nativeReturned.get_future();
    futureNativeEntered = &nativeEntered;
    futureNativeReturned = &nativeReturned;
    shutdown1459BlockedTid.store(0);
    futureCallbackReturned.store(false);
    auto task = RunCJTask(NativeReadValueTask, &input);
    GC_EXPECT_TRUE(task != nullptr);
    const bool entered = entry.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    ReleaseHandle(task);
    // Ownership was consumed. Do not query/release this handle again.
    const bool returnedBeforeUnblock = futureCallbackReturned.load(std::memory_order_acquire);
    const int written = write(descriptors[1], "x", 1);
    const bool returned = completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    std::fprintf(stderr, "FUTURE_RELEASE_TARGET entered=%d premature=%d callback_returned=%d\n",
                 entered, returnedBeforeUnblock, returned);
    GC_EXPECT_TRUE(entered && !returnedBeforeUnblock && written == 1 && returned);
    FinishShutdownRuntime();
    close(descriptors[0]);
    close(descriptors[1]);
}

GC_RUNTIME_OTHER_VM_TEST(RuntimeFuture, InvalidArgumentsDoNotConsumeHandle)
{
    InitializeShutdownRuntime();
    int value = 42;
    auto task = RunCJTask([](void* input) -> void* { return input; }, &value);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = nullptr;
    const int nullHandle = GetTaskRet(nullptr, &result);
    const int nullResult = GetTaskRet(task, nullptr);
    const int valid = GetTaskRet(task, &result);
    ReleaseHandle(task);
    std::fprintf(stderr, "FUTURE_ARGUMENT_CONTROL null_handle=%d null_result=%d valid=%d\n",
                 nullHandle, nullResult, valid);
    GC_EXPECT_EQ(nullHandle, E_ARGS);
    GC_EXPECT_EQ(nullResult, E_ARGS);
    GC_EXPECT_EQ(valid, E_OK);
    FinishShutdownRuntime();
}

GC_RUNTIME_OTHER_VM_TEST(Shutdown1459, NativeCallReturnAllowsCarrierJoin)
{
    InitializeShutdownRuntime();
    int descriptors[2];
    GC_EXPECT_EQ(pipe(descriptors), 0);
    GC_EXPECT_EQ(write(descriptors[1], "x", 1), 1);
    NativeReadInput input{descriptors[0]};
    auto task = RunCJTask(NativeReadTask, &input);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = reinterpret_cast<void*>(1);
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    GC_EXPECT_TRUE(result == nullptr);
    ReleaseHandle(task);
    close(descriptors[0]);
    close(descriptors[1]);
    FinishShutdownRuntime();
}

GC_RUNTIME_OTHER_VM_TEST(Shutdown1459, UnreturnedNativeCallRetainsStorage)
{
    InitializeShutdownRuntime();
    int descriptors[2];
    GC_EXPECT_EQ(pipe(descriptors), 0);
    NativeReadInput input{descriptors[0]};
    auto task = RunCJTask(NativeReadTask, &input);
    GC_EXPECT_TRUE(task != nullptr);
    bool inRead = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        long tid = shutdown1459BlockedTid.load();
        if (tid > 0) {
            std::ifstream state("/proc/self/task/" + std::to_string(tid) + "/syscall");
            long number = -1;
            if (state >> number && number == SYS_read) {
                inRead = true;
                break;
            }
        }
        std::this_thread::yield();
    }
    GC_EXPECT_TRUE(inRead);
    std::fprintf(stderr, "SHUTDOWN1459_NATIVE_READ_HELD tid=%ld syscall=%d\n",
                 shutdown1459BlockedTid.load(), SYS_read);
    auto* before = MapleRuntime::Runtime::CurrentRef();
    auto rc = FiniCJRuntime();
    bool retained = before != nullptr && MapleRuntime::Runtime::CurrentRef() == before;
    std::fprintf(stderr, "SHUTDOWN1459_BLOCKED_TARGET fini_rc=%d runtime_retained=%d\n", rc, retained);
    GC_EXPECT_EQ(rc, E_FAILED);
    GC_EXPECT_TRUE(retained);
    // The outstanding native call and its input are retained until process exit.
    // Do not manufacture a successful destroy or join by releasing this pipe.
}
#endif
