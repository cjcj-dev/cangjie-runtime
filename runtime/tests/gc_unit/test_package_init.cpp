// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <thread>
#include "Cangjie.h"
#include "Cki.h"
#include "Common/Runtime.h"
#include "Common/ScopedObjectAccess.h"
#include "Concurrency/ConcurrencyModel.h"
#include "ExceptionManager.inline.h"
#include "Heap/Heap.h"
#include "Loader/BinaryFile/CjFile/CjFile.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "Loader/PackageInit.h"
#include "LoaderManager.h"
#include "schedule.h"
#include "waitqueue.h"

using namespace MapleRuntime;
namespace {
using Result = PackageInitResult;
constexpr uint32_t Code(Result value) { return static_cast<uint32_t>(value); }
void PackageA() {}
void PackageB() {}
void UnitA() {}
void UnitB() {}
void Aggregate() {}

// Real CJFile metadata in the executable image, loaded through the product
// catalog. These are native ABI consumers; compiler-generated consumers are #48.
struct Metadata {
    CJFileHeader header {};
    CJGCFlagsTable flags { 1, 1, 0 };
    Uptr entries[2] { reinterpret_cast<Uptr>(&PackageA), reinterpret_cast<Uptr>(&PackageB) };
};
Metadata metadata;
CJFile* image;

bool Await(const std::atomic<bool>& flag)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= until) { return false; }
        std::this_thread::yield();
    }
    return true;
}
void Target(const char* name, bool passed)
{
    std::fprintf(stderr, "PACKAGE_INIT_TARGET %s executed=1 pass=%d\n", name, passed);
    std::fflush(stderr);
    if (!passed) { std::_Exit(1); }
}
void RegisterImage()
{
    metadata.header.cJFileSize = sizeof(metadata);
    metadata.header.tables[GC_FLAGS_TABLE] = { offsetof(Metadata, flags), sizeof(metadata.flags) };
    metadata.header.tables[GLOBAL_INIT_FUNC_TABLE] = { offsetof(Metadata, entries), sizeof(metadata.entries) };
    image = new CJFile(CString("package-init-main"), reinterpret_cast<Uptr>(&metadata));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(image);
    loader->RegisterLoadFile(reinterpret_cast<Uptr>(&metadata));
}
void Init()
{
    (void)setenv("cjProcessorNum", "1", 1);
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    Target("runtime-start", InitCJRuntime(&param) == E_OK);
    RegisterImage();
}

struct Task {
    // The scheduler's root visitor consumes LWTData. All three managed slots
    // are null; the callback/context follow them as native data.
    LWTData roots {};
    void (*body)(void*);
    void* context;
};
void* Run(void* argument, unsigned int)
{
    auto* task = static_cast<Task*>(argument);
    Mutator::GetMutator()->SetManagedContext(false);
    task->body(task->context);
    return nullptr; // Real scheduler SCHD_DESTROY_MUTATOR performs owner cleanup.
}
void Start(void (*body)(void*), void* context = nullptr)
{
    Task task {};
    task.body = body;
    task.context = context;
    CJThreadAttr attr;
    CJThreadAttrInit(&attr);
    CJThreadAttrCjFromCSet(&attr, true);
    auto scheduler = Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    Target("scheduler-admission", CJThreadNew(scheduler, &attr, Run, &task, sizeof(task)) != nullptr);
}
uint32_t Begin(const void* package, const void* unit, uint32_t phase, void** token)
{
    return MCC_PackageInitBegin(package, unit, phase, token);
}
const void* P() { return reinterpret_cast<const void*>(&PackageA); }
const void* U() { return reinterpret_cast<const void*>(&UnitA); }
const void* V() { return reinterpret_cast<const void*>(&UnitB); }

struct Completion {
    Waitqueue release {};
    std::atomic<bool> ownerStarted { false }, waiterStarted { false }, waiterDone { false };
    std::atomic<bool> witnessDone { false }, finish { false };
    uint32_t waiterResult = 99;
    int payload = 0;
    bool fail = false;
    bool abandon = false;
    Completion() { Target("fixture-waitqueue", WaitqueueNew(&release) == 0); }
};
bool Released(void* p) { return static_cast<Completion*>(p)->finish.load(std::memory_order_acquire); }
void Owner(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = nullptr;
    Target("owner-execute", Begin(P(), U(), 0, &token) == Code(Result::Execute) && token != nullptr);
    c.ownerStarted.store(true, std::memory_order_release);
    WaitqueuePark(&c.release, LLONG_MAX, Released, &c, false);
    c.payload = 73;
    if (c.abandon) { return; }
    if (c.fail) {
        // Preserve the exact native exception wrapper state, including its
        // pending managed reference. No GC is requested during this snapshot.
        auto& wrapper = Mutator::GetMutator()->GetExceptionWrapper();
        unsigned char before[sizeof(wrapper)];
        std::memcpy(before, &wrapper, sizeof(wrapper));
        MCC_PackageInitFail(token, 1);
        Target("pending-exception-unchanged", std::memcmp(before, &wrapper, sizeof(wrapper)) == 0);
    } else {
        MCC_PackageInitComplete(token);
    }
}
void Waiter(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = reinterpret_cast<void*>(1);
    c.waiterStarted.store(true, std::memory_order_release);
    c.waiterResult = Begin(P(), U(), 0, &token);
    Target("waiter-token-empty", token == nullptr);
    if (!c.fail && !c.abandon) { Target("all-body-writes-visible", c.payload == 73); }
    c.waiterDone.store(true, std::memory_order_release);
}
void Witness(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    // With one worker this task can run only after the preceding waiter has
    // yielded or returned. A real collection also rendezvous with both parked
    // logical threads; the task data has no native pointer in managed slots.
    Target("not-ready-before-complete", !c.waiterDone.load(std::memory_order_acquire));
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    c.witnessDone.store(true, std::memory_order_release);
}
void Repeat(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = nullptr;
    const uint32_t expected = Code(c.fail || c.abandon ? Result::Failed : Result::Ready);
    Target("sticky-terminal", Begin(P(), U(), 0, &token) == expected && token == nullptr);
    c.witnessDone.store(true, std::memory_order_release);
}
void CompletionCase(bool fail, bool abandon)
{
    Init();
    Completion c;
    c.fail = fail;
    c.abandon = abandon;
    Start(Owner, &c);
    Target("owner-reached-body", Await(c.ownerStarted));
    Start(Waiter, &c);
    Target("waiter-reached-begin", Await(c.waiterStarted));
    Start(Witness, &c);
    Target("one-worker-gc-completed", Await(c.witnessDone));
    c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&c.release, nullptr, nullptr);
    Target("completion-notified", Await(c.waiterDone));
    Target("terminal-result", c.waiterResult == Code(fail || abandon ? Result::Failed : Result::Ready));
    c.witnessDone.store(false, std::memory_order_release);
    Start(Repeat, &c);
    Target("repeat-completed", Await(c.witnessDone));
    // Fini joins worker teardown before stack-owned fixture data is released.
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}

void Isolation(void* p)
{
    void* token = nullptr;
    Target("isolation-first-execute", Begin(P(), U(), 0, &token) == Code(Result::Execute));
    void* reentrant = reinterpret_cast<void*>(1);
    Target("reentrant-distinct", Begin(P(), U(), 0, &reentrant) == Code(Result::Reentrant) && !reentrant);
    MCC_PackageInitComplete(token);
    const void* packages[] = { P(), P(), reinterpret_cast<const void*>(&PackageB) };
    const void* units[] = { V(), U(), U() };
    const uint32_t phases[] = { 0, 1, 0 };
    for (size_t i = 0; i != 3; ++i) {
        token = nullptr;
        const auto result = Begin(packages[i], units[i], phases[i], &token);
        std::fprintf(stderr, "PACKAGE_INIT_KEY index=%zu result=%u\n", i, result);
        Target("package-phase-unit-isolation", result == Code(Result::Execute) && token != nullptr);
        MCC_PackageInitComplete(token);
    }
    token = nullptr;
    Target("aggregate-independent", Begin(P(), reinterpret_cast<const void*>(&Aggregate), 0, &token) ==
        Code(Result::Execute));
    MCC_PackageInitComplete(token);
    static_cast<std::atomic<bool>*>(p)->store(true, std::memory_order_release);
}

struct Cycle {
    Waitqueue release {};
    std::atomic<bool> first { false }, second { false }, go { false }, done { false };
};
bool CycleGo(void* p) { return static_cast<Cycle*>(p)->go.load(std::memory_order_acquire); }
void CycleFirst(void* p)
{
    auto& c = *static_cast<Cycle*>(p);
    void* owner = nullptr;
    Target("cycle-first-execute", Begin(P(), U(), 0, &owner) == Code(Result::Execute));
    c.first.store(true, std::memory_order_release);
    WaitqueuePark(&c.release, LLONG_MAX, CycleGo, &c, false);
    void* dependency = nullptr;
    Target("cycle-first-terminal", Begin(P(), V(), 0, &dependency) == Code(Result::Cycle));
    MCC_PackageInitFail(owner, 1);
}
void CycleSecond(void* p)
{
    auto& c = *static_cast<Cycle*>(p);
    void* owner = nullptr;
    Target("cycle-second-execute", Begin(P(), V(), 0, &owner) == Code(Result::Execute));
    c.second.store(true, std::memory_order_release);
    void* dependency = nullptr;
    Target("cycle-propagated-failure", Begin(P(), U(), 0, &dependency) == Code(Result::Failed));
    MCC_PackageInitFail(owner, 1);
    c.done.store(true, std::memory_order_release);
}
void CycleWitness(void* p)
{
    auto& c = *static_cast<Cycle*>(p);
    c.go.store(true, std::memory_order_release);
    WaitqueueWakeAll(&c.release, nullptr, nullptr);
}

void Lookup(void* p)
{
    void* token = nullptr;
    Target("code-to-real-basefile", Begin(P(), U(), 0, &token) == Code(Result::Execute));
    MCC_PackageInitComplete(token);
    Target("metadata-is-not-code", Begin(&metadata, U(), 0, &token) == Code(Result::Unavailable));
    Target("other-image-unit-rejected", Begin(P(), reinterpret_cast<const void*>(&MCC_PackageInitBegin), 0,
                                            &token) == Code(Result::Unavailable));
    static_cast<std::atomic<bool>*>(p)->store(true, std::memory_order_release);
}
} // namespace

GC_OTHER_VM_TEST(PackageInit, CompletionWaitsForBodyAndHandshake) { CompletionCase(false, false); }
GC_OTHER_VM_TEST(PackageInit, FailureWakesAndRemainsSticky) { CompletionCase(true, false); }
GC_OTHER_VM_TEST(PackageInit, OwnerExitWakesAndFails) { CompletionCase(false, true); }
GC_OTHER_VM_TEST(PackageInit, PackagePhaseUnitAndReentry)
{
    Init();
    std::atomic<bool> done { false };
    Start(Isolation, &done);
    Target("isolation-completed", Await(done));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_OTHER_VM_TEST(PackageInit, CrossOwnerCycleHasTerminalResult)
{
    Init();
    Cycle c;
    Target("fixture-waitqueue", WaitqueueNew(&c.release) == 0);
    Start(CycleFirst, &c);
    Target("cycle-first-started", Await(c.first));
    Start(CycleSecond, &c);
    Target("cycle-second-started", Await(c.second));
    Start(CycleWitness, &c);
    Target("cycle-completed", Await(c.done));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}
GC_OTHER_VM_TEST(PackageInit, RegisteredMainCodeAndImageGeneration)
{
    Init();
    for (int generation = 0; generation != 2; ++generation) {
        std::atomic<bool> done { false };
        Start(Lookup, &done);
        Target("lookup-completed", Await(done));
        if (generation == 0) {
            auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
            loader->RemoveLoadedFiles(image);
            RegisterImage();
        }
    }
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_TEST(PackageInit, UnattachedNativeUnavailable)
{
    void* token = reinterpret_cast<void*>(1);
    GC_EXPECT_EQ(Begin(P(), U(), 0, &token), Code(Result::Unavailable));
    GC_EXPECT_TRUE(token == nullptr);
}
GC_TEST(PackageInit, AbortUsesNativeExit70)
{
    pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) { MCC_PackageInitAbort(P(), U(), 0, Code(Result::Failed)); }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 70);
}
#endif
