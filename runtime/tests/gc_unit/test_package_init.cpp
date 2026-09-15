// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#define MRT_USE_CJTHREAD_RENAME 1
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include "Cangjie.h"
#include "Concurrency/Concurrency.h"
#include "Common/Runtime.h"
#include "Common/ScopedObjectAccess.h"
#include "Concurrency/ConcurrencyModel.h"
#include "ExceptionManager.inline.h"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/MObject.h"
#include "Loader/BinaryFile/CjFile/CjFile.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "Loader/PackageInit.h"
#include "Loader/PackageInitTest.h"
#include "LoaderManager.h"
#include "schedule.h"
#include "waitqueue.h"

using namespace MapleRuntime;
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" bool MRT_NewForeignCJThread();
extern "C" bool MRT_EndForeignCJThread();
}
namespace {
using Result = PackageInitResult;
constexpr uint32_t Code(Result value) { return static_cast<uint32_t>(value); }
void (*packageBody)() = nullptr;
void PackageA() { if (packageBody != nullptr) { packageBody(); } }
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
    void* nativeToken = reinterpret_cast<void*>(1);
    Target("unattached-after-runtime-init", MCC_PackageInitBegin(reinterpret_cast<const void*>(&PackageA),
        reinterpret_cast<const void*>(&UnitA), 0, &nativeToken) == Code(Result::Unavailable) && nativeToken == nullptr);
}

struct Task {
    void (*body)(void*);
    void* context;
};
void* Run(void* argument, unsigned int)
{
    // LWTData::fn is explicitly native and excluded by MRT_VisitorCaller;
    // the three managed slots remain null. Stay within COARGS_SIZE_MAX.
    auto* task = static_cast<Task*>(static_cast<LWTData*>(argument)->fn);
    const Task work = *task;
    delete task;
    Mutator::GetMutator()->SetManagedContext(false);
    work.body(work.context);
    return nullptr; // Real scheduler SCHD_DESTROY_MUTATOR performs owner cleanup.
}
void Start(void (*body)(void*), void* context = nullptr)
{
    LWTData roots {};
    roots.fn = new Task { body, context };
    CJThreadAttr attr;
    CJThreadAttrInit(&attr);
    CJThreadAttrCjFromCSet(&attr, true);
    auto scheduler = Runtime::Current().GetConcurrencyModel().GetThreadScheduler();
    Target("scheduler-admission", CJThreadNew(scheduler, &attr, Run, &roots, sizeof(roots)) != nullptr);
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
    int observedPayload = 0;
    bool fail = false;
    bool abandon = false;
    bool aggregate = false;
    void* ownerToken = nullptr;
    Completion() { Target("fixture-waitqueue", WaitqueueNew(&release) == 0); }
};
bool Released(void* p) { return static_cast<Completion*>(p)->finish.load(std::memory_order_acquire); }
void Owner(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = nullptr;
    Target("owner-execute", Begin(P(), U(), 0, &token) == Code(Result::Execute) && token != nullptr);
    c.ownerToken = token;
    c.ownerStarted.store(true, std::memory_order_release);
    WaitqueuePark(&c.release, LLONG_MAX, Released, &c, false);
    c.payload = 73;
    if (c.abandon) { return; }
    if (c.fail) {
        // Preserve the exact native exception wrapper state, including its
        // pending managed reference. No GC is requested during this snapshot.
        auto& wrapper = Mutator::GetMutator()->GetExceptionWrapper();
        alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)] {};
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(64);
        auto* exception = MCC_NewObject(type, 64 + TYPEINFO_PTR_SIZE);
        Target("pending-exception-positive", exception != nullptr);
        wrapper.SetExceptionRef(exception);
        wrapper.SetTypeIndex(19);
        MCC_PackageInitFail(token, 1);
        Target("pending-exception-unchanged", wrapper.GetExceptionRef() != nullptr &&
               wrapper.GetExceptionRef()->GetTypeInfo() == type && wrapper.GetTypeIndex() == 19);
        ExceptionManager::ClearPendingException();
    } else {
        MCC_PackageInitComplete(token);
    }
}
void Waiter(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = reinterpret_cast<void*>(1);
    c.waiterStarted.store(true, std::memory_order_release);
    c.waiterResult = Begin(P(), c.aggregate ? reinterpret_cast<const void*>(&Aggregate) : U(), 0, &token);
    Target("waiter-token-empty", token == nullptr);
    if (!c.fail && !c.abandon) { c.observedPayload = c.payload; }
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
void AggregateOwner(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* aggregate = nullptr;
    Target("aggregate-execute", Begin(P(), reinterpret_cast<const void*>(&Aggregate), 0, &aggregate) ==
        Code(Result::Execute));
    void* dependency = nullptr;
    Target("aggregate-dependency-complete", Begin(P(), U(), 0, &dependency) == Code(Result::Ready));
    Target("aggregate-sees-body", c.payload == 73);
    MCC_PackageInitComplete(aggregate);
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
    if (!fail && !abandon) { Target("all-body-writes-visible", c.observedPayload == 73); }
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

struct UnloadRace {
    Completion c;
    std::atomic<bool> dependencyDone { false }, unloadStarted { false }, unloadDone { false };
};
void DependencyOwner(void* p)
{
    auto& race = *static_cast<UnloadRace*>(p);
    void* owner = nullptr;
    Target("unload-owner-execute", Begin(P(), U(), 0, &owner) == Code(Result::Execute));
    race.c.ownerStarted.store(true, std::memory_order_release);
    WaitqueuePark(&race.c.release, LLONG_MAX, Released, &race.c, false);
    void* dependency = nullptr;
    Target("dependency-admitted-during-direct-wait", Begin(P(), V(), 0, &dependency) == Code(Result::Execute));
    MCC_PackageInitComplete(dependency);
    race.dependencyDone.store(true, std::memory_order_release);
    Target("pending-prevents-purge", !race.unloadDone.load(std::memory_order_acquire));
    MCC_PackageInitComplete(owner);
}

struct AdmissionRace {
    std::atomic<bool> beginStarted { false }, done { false }, witness { false };
};
void AdmissionCaller(void* p)
{
    auto& race = *static_cast<AdmissionRace*>(p);
    void* token = nullptr;
    race.beginStarted.store(true, std::memory_order_release);
    Target("lock-competition-is-not-unavailable", Begin(P(), U(), 0, &token) == Code(Result::Execute));
    MCC_PackageInitComplete(token);
    race.done.store(true, std::memory_order_release);
}
void AdmissionWitness(void* p)
{
    auto& race = *static_cast<AdmissionRace*>(p);
    Target("admission-wait-yields-worker", !race.done.load(std::memory_order_acquire));
    Heap::GetHeap().GetCollector().RequestGC(GC_REASON_YOUNG, false);
    race.witness.store(true, std::memory_order_release);
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
GC_OTHER_VM_TEST(PackageInit, AggregateWaitsForLastUnit)
{
    Init();
    Completion c;
    c.aggregate = true;
    Start(Owner, &c);
    Target("owner-reached-body", Await(c.ownerStarted));
    Start(AggregateOwner, &c);
    Start(Waiter, &c);
    Target("waiter-reached-begin", Await(c.waiterStarted));
    Start(Witness, &c);
    Target("aggregate-witness-completed", Await(c.witnessDone));
    c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&c.release, nullptr, nullptr);
    Target("aggregate-completed", Await(c.waiterDone) && c.waiterResult == Code(Result::Ready));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}
GC_OTHER_VM_TEST(PackageInit, DirectUnloadAllowsOwnerDependency)
{
    Init();
    UnloadRace race;
    Start(DependencyOwner, &race);
    Target("owner-reached-body", Await(race.c.ownerStarted));
    Start([](void* p) {
        auto& r = *static_cast<UnloadRace*>(p);
        r.unloadStarted.store(true, std::memory_order_release);
        static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader())->RemoveLoadedFiles(image);
        r.unloadDone.store(true, std::memory_order_release);
    }, &race);
    Target("direct-unload-started", Await(race.unloadStarted));
    Start([](void* p) {
        auto& r = *static_cast<UnloadRace*>(p);
        Target("direct-wait-yields-worker", !r.unloadDone.load(std::memory_order_acquire));
        r.c.finish.store(true, std::memory_order_release);
        WaitqueueWakeAll(&r.c.release, nullptr, nullptr);
    }, &race);
    Target("dependency-completed", Await(race.dependencyDone));
    Target("direct-unload-completed", Await(race.unloadDone));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&race.c.release);
}
GC_OTHER_VM_TEST(PackageInit, AdmissionCompetitionParksAndRetries)
{
    Init();
    AdmissionRace race;
    {
        ElfUnloadQuiescence::TaskAdmissionScope exclusive;
        Start(AdmissionCaller, &race);
        Target("admission-begin-started", Await(race.beginStarted));
        Start(AdmissionWitness, &race);
        Target("admission-witness-completed", Await(race.witness));
    }
    Target("admission-release-notified", Await(race.done));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_OTHER_VM_TEST(PackageInit, ForeignCJThreadWaitsForCompletion)
{
    Init();
    Completion c;
    Start(Owner, &c);
    Target("owner-reached-body", Await(c.ownerStarted));
    std::atomic<Mutator*> foreignMutator { nullptr };
    std::atomic<bool> mayDetach { false };
    std::thread foreign([&]() {
        Target("foreign-attach", MRT_NewForeignCJThread());
        foreignMutator.store(Mutator::GetMutator(), std::memory_order_release);
        Mutator::GetMutator()->SetManagedContext(false);
        Waiter(&c);
        {
            ScopedEnterSaferegion safe(false);
            while (!mayDetach.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        }
        Target("foreign-detach", MRT_EndForeignCJThread());
    });
    Target("foreign-waiter-started", Await(c.waiterStarted));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto* parked = foreignMutator.load(std::memory_order_acquire);
    while (parked->GetEpochHandshakeLifecycle() != Mutator::EPOCH_HANDSHAKE_PARKED &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    Target("foreign-really-parked", parked->GetEpochHandshakeLifecycle() == Mutator::EPOCH_HANDSHAKE_PARKED &&
           !c.waiterDone.load(std::memory_order_acquire));
    c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&c.release, nullptr, nullptr);
    Target("foreign-waiter-completed", Await(c.waiterDone));
    mayDetach.store(true, std::memory_order_release);
    foreign.join();
    Target("all-body-writes-visible", c.observedPayload == 73);
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}
GC_OTHER_VM_TEST(PackageInit, LibraryCodeUnloadReloadHasNewState)
{
    Init();
    char executable[4096] {};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    Target("library-fixture-executable", length > 0);
    std::string path(executable, static_cast<size_t>(length));
    path = path.substr(0, path.find_last_of('/') + 1) + "libcj_package_init_fixture.so";
    for (int generation = 0; generation != 2; ++generation) {
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        Target("library-fixture-open", library != nullptr);
        auto getMetadata = reinterpret_cast<void* (*)()>(dlsym(library, "PackageInitImageMetadata"));
        struct Context {
            const void* package;
            const void* unit;
            std::atomic<bool> done { false };
        } context { dlsym(library, "PackageInitImagePackage"), dlsym(library, "PackageInitImageUnit") };
        Target("library-fixture-symbols", getMetadata && context.package && context.unit);
        auto* file = new CJFile(CString("package-init-library"), reinterpret_cast<Uptr>(getMetadata()));
        auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
        loader->AddLoadedFiles(file);
        loader->RegisterLoadFile(file->GetFileMetaAddr());
        Start([](void* argument) {
            auto& c = *static_cast<Context*>(argument);
            void* token = nullptr;
            Target("library-new-generation-execute", Begin(c.package, c.unit, 0, &token) == Code(Result::Execute));
            MCC_PackageInitComplete(token);
            Target("library-generation-ready", Begin(c.package, c.unit, 0, &token) == Code(Result::Ready));
            c.done.store(true, std::memory_order_release);
        }, &context);
        Target("library-initialized", Await(context.done));
        loader->RemoveLoadedFiles(file);
        Target("library-fixture-closed", dlclose(library) == 0);
    }
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
namespace {
std::atomic<bool> nativeFiniEntered { false };
void NativeFiniNotice() { nativeFiniEntered.store(true, std::memory_order_release); }
}
GC_OTHER_VM_TEST(PackageInit, NativeDlcloseAllowsDependency)
{
    Init();
    char executable[4096] {};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    Target("library-fixture-executable", length > 0);
    std::string path(executable, static_cast<size_t>(length));
    path = path.substr(0, path.find_last_of('/') + 1) + "libcj_package_init_fixture.so";
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    Target("library-fixture-open", library != nullptr);
    auto getMetadata = reinterpret_cast<void* (*)()>(dlsym(library, "PackageInitImageMetadata"));
    auto armUnload = reinterpret_cast<void (*)(void (*)())>(dlsym(library, "PackageInitImageArmUnload"));
    struct Context {
        Completion c;
        const void* package;
        const void* unit;
        std::atomic<bool> done { false }, closed { false };
    } context;
    context.package = dlsym(library, "PackageInitImagePackage");
    context.unit = dlsym(library, "PackageInitImageUnit");
    Target("library-fixture-symbols", getMetadata && armUnload && context.package && context.unit);
    auto* file = new CJFile(CString("package-init-library"), reinterpret_cast<Uptr>(getMetadata()));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(file);
    loader->RegisterLoadFile(file->GetFileMetaAddr());
    armUnload(NativeFiniNotice);
    Start([](void* argument) {
        auto& c = *static_cast<Context*>(argument);
        void* owner = nullptr;
        Target("native-close-owner", Begin(c.package, c.unit, 0, &owner) == Code(Result::Execute));
        c.c.ownerStarted.store(true, std::memory_order_release);
        WaitqueuePark(&c.c.release, LLONG_MAX, Released, &c.c, false);
        void* dependency = nullptr;
        Target("native-close-dependency-ready", Begin(P(), V(), 0, &dependency) == Code(Result::Execute));
        MCC_PackageInitComplete(dependency);
        MCC_PackageInitComplete(owner);
        c.done.store(true, std::memory_order_release);
    }, &context);
    Target("native-close-owner-started", Await(context.c.ownerStarted));
    std::thread closer([&]() {
        Target("native-close-return", dlclose(library) == 0);
        context.closed.store(true, std::memory_order_release);
    });
    Target("native-fini-entered", Await(nativeFiniEntered));
    context.c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&context.c.release, nullptr, nullptr);
    Target("native-close-dependency-completed", Await(context.done));
    Target("native-close-completed", Await(context.closed));
    closer.join();
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&context.c.release);
}
#ifdef MRT_TESTABLE_INTERNALS
namespace {
Completion* libInitControl;
void LibInitBody()
{
    Mutator::GetMutator()->SetManagedContext(false);
    void* owner = nullptr;
    Target("libinit-body-execute", Begin(P(), U(), 0, &owner) == Code(Result::Execute));
    libInitControl->ownerStarted.store(true, std::memory_order_release);
    WaitqueuePark(&libInitControl->release, LLONG_MAX, Released, libInitControl, false);
    void* dependency = nullptr;
    Target("libinit-dependency-execute", Begin(P(), V(), 0, &dependency) == Code(Result::Execute));
    MCC_PackageInitComplete(dependency);
    MCC_PackageInitComplete(owner);
}
}
GC_OTHER_VM_TEST(PackageInit, LibInitBodyDoesNotHoldGlobalReader)
{
    Init();
    Completion control;
    libInitControl = &control;
    packageBody = LibInitBody;
    char executable[4096] {};
    const auto length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    Target("library-fixture-executable", length > 0);
    std::string path(executable, static_cast<size_t>(length));
    path = path.substr(0, path.find_last_of('/') + 1) + "libcj_package_init_fixture.so";
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    Target("unrelated-library-open", library != nullptr);
    auto getMetadata = reinterpret_cast<void* (*)()>(dlsym(library, "PackageInitImageMetadata"));
    Target("unrelated-library-metadata", getMetadata != nullptr);
    auto* unrelated = new CJFile(CString("unrelated-package-init-library"), reinterpret_cast<Uptr>(getMetadata()));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(unrelated);
    loader->RegisterLoadFile(unrelated->GetFileMetaAddr());
    Start([](void* argument) {
        auto& c = *static_cast<Completion*>(argument);
        Target("libinit-return", LoaderManager::GetInstance()->LibInit("package-init-main"));
        c.waiterDone.store(true, std::memory_order_release);
    }, &control);
    Target("libinit-body-started", Await(control.ownerStarted));
    // A separate short native reader establishes the real loader drain point.
    // The existing testable observation does not alter the product protocol.
    auto reader = std::make_unique<ElfUnloadQuiescence::ReadScope>();
    std::atomic<bool> removed { false };
    std::thread unload([&]() {
        loader->RemoveLoadedFiles(unrelated);
        removed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ElfUnloadQuiescence::IsUnloadPendingForTesting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Target("unrelated-unload-reached-drain", ElfUnloadQuiescence::IsUnloadPendingForTesting());
    control.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&control.release, nullptr, nullptr);
    reader.reset();
    Target("libinit-and-unrelated-unload-progress", Await(control.waiterDone));
    Target("unrelated-unload-completed", Await(removed));
    unload.join();
    Target("unrelated-library-close", dlclose(library) == 0);
    packageBody = nullptr;
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&control.release);
}
#endif
#ifdef MRT_TESTABLE_INTERNALS
GC_OTHER_VM_TEST(PackageInit, CompletePauseUsesLogicalWaitAndExactIdentity)
{
    Init();
    Target("complete-pause-arm", MRT_PackageInitArmCompletePause(P(), U(), 0));
    std::atomic<bool> unrelatedDone { false };
    Start([](void* argument) {
        void* token = nullptr;
        Target("pause-control-execute", Begin(P(), V(), 0, &token) == Code(Result::Execute));
        MCC_PackageInitComplete(token);
        static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
    }, &unrelatedDone);
    Target("pause-control-unaffected", Await(unrelatedDone) && !MRT_PackageInitCompletePauseReached());
    Completion c;
    c.finish.store(true, std::memory_order_release);
    Start(Owner, &c);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!MRT_PackageInitCompletePauseReached() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Target("product-complete-reached-pause", MRT_PackageInitCompletePauseReached());
    Start(Waiter, &c);
    Target("pause-waiter-started", Await(c.waiterStarted));
    const auto waiterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!MRT_PackageInitHasWaitingCaller(P(), U(), 0) && !c.waiterDone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < waiterDeadline) { std::this_thread::yield(); }
    Target("second-caller-in-product-wait-graph", MRT_PackageInitHasWaitingCaller(P(), U(), 0) &&
           !MRT_PackageInitHasWaitingCaller(P(), V(), 0) && !MRT_PackageInitHasWaitingCaller(P(), U(), 1));
    Start(Witness, &c);
    Target("complete-pause-cooperates-with-gc", Await(c.witnessDone));
    MRT_PackageInitReleaseCompletePause();
    Target("complete-pause-release", Await(c.waiterDone) && c.waiterResult == Code(Result::Ready));
    Target("product-wait-graph-cleared", !MRT_PackageInitHasWaitingCaller(P(), U(), 0));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}
#endif

namespace {
void CheckTokenMisuse(bool nonOwner)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        (void)dup2(output[1], STDERR_FILENO);
        (void)dup2(output[1], STDOUT_FILENO);
        close(output[1]);
        Init();
        Completion c;
        if (nonOwner) {
            Start(Owner, &c);
            Target("misuse-owner-started", Await(c.ownerStarted));
            Start([](void* p) {
                auto& c = *static_cast<Completion*>(p);
                std::fprintf(stderr, "PACKAGE_INIT_MISUSE_TARGET non-owner\n");
                MCC_PackageInitComplete(c.ownerToken);
                std::_Exit(77);
            }, &c);
        } else {
            Start([](void*) {
                void* token = nullptr;
                Target("misuse-first-execute", Begin(P(), U(), 0, &token) == Code(Result::Execute));
                MCC_PackageInitComplete(token);
                std::fprintf(stderr, "PACKAGE_INIT_MISUSE_TARGET duplicate\n");
                MCC_PackageInitFail(token, 1);
                std::_Exit(77);
            });
        }
        std::atomic<bool> never { false };
        (void)Await(never);
        std::_Exit(78);
    }
    close(output[1]);
    std::string transcript;
    char bytes[1024];
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof(bytes))) > 0) { transcript.append(bytes, static_cast<size_t>(count)); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const char* guard = nonOwner ? "package initializer token belongs to another logical CJThread" :
        "package initializer token is not active";
    const bool target = transcript.find("PACKAGE_INIT_MISUSE_TARGET") != std::string::npos &&
        transcript.find(guard) != std::string::npos;
    std::fprintf(stderr, "PACKAGE_INIT_TOKEN_GUARD nonOwner=%d status=%d target=%d\n", nonOwner, status, target);
    GC_EXPECT_TRUE(target);
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
}
GC_TEST(PackageInit, DuplicateTokenRejected) { CheckTokenMisuse(false); }
GC_TEST(PackageInit, NonOwnerTokenRejected) { CheckTokenMisuse(true); }
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
