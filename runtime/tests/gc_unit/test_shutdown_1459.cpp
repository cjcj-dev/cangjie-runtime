#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadSMR.h"
#include "Mutator/VMExit.h"
#include "Mutator/SafepointSynchronize.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
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
std::atomic<MapleRuntime::Mutator*> shutdown1459BlockedMutator{nullptr};
namespace {
struct NativeReadInput { int descriptor; };
void* NativeRead(void* arg)
{
    shutdown1459BlockedMutator.store(MapleRuntime::Mutator::GetMutator(), std::memory_order_release);
    shutdown1459BlockedTid.store(syscall(SYS_gettid));
    char byte = 0;
    int rc = CJ_SyscallRead(static_cast<NativeReadInput*>(arg)->descriptor, &byte, 1);
    return reinterpret_cast<void*>(rc == 1 && byte == 'x' ? 0 : 1);
}
void* NativeReadTask(void* arg)
{
    return Shutdown1459CallNative(arg, NativeRead);
}
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
    auto* blocked = shutdown1459BlockedMutator.load(std::memory_order_acquire);
    GC_EXPECT_TRUE(blocked != nullptr);
    const auto rc = FiniCJRuntime();
    const bool deleted = MapleRuntime::Runtime::CurrentRef() == nullptr;
    // threads.cpp:995-1027 / vmOperations.cpp:451-468: logical success may
    // retain a live native thread, but must not retain the Runtime facade.
    MapleRuntime::ThreadsListHandle threads;
    bool retained = false;
    for (size_t i = 0; i < threads.length(); ++i) {
        if (threads.thread_at(i) == blocked) {
            retained = blocked->IsTerminatedByVM() && !blocked->IsGCDetached() &&
                       blocked->GetCjthreadPtr() != nullptr &&
                       blocked->GetGCData().storeBarrierBuffer != nullptr &&
                       MapleRuntime::VM_Exit::is_native(*blocked);
        }
    }
    const bool terminal = MapleRuntime::VM_Exit::exited() &&
                          MapleRuntime::SafepointSynchronize::is_at_safepoint();
    std::fprintf(stderr,
        "SHUTDOWN1459_BLOCKED_TARGET fini_rc=%d runtime_deleted=%d native_storage_retained=%d terminal=%d\n",
        rc, deleted, retained, terminal);
    GC_EXPECT_EQ(rc, E_OK);
    GC_EXPECT_TRUE(deleted && retained && terminal);
    // Keep the pipe blocked. Grok separately observes no join/destruction and
    // the actual late-return barrier; process completion alone is not proof.
}
#endif
