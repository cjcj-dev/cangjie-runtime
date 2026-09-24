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
#include "Loader/ElfUnloadQuiescence.h"
#include "Loader/PackageInit.h"
#include "loader_access_test.hpp"
#include "LoaderManager.h"
#include "Mutator/Handshake.h"
#include "schedule.h"
#include "waitqueue.h"

// The test executable replaces the external platform service, not any runtime
// function. Product UnloadLibrary still owns preflight, STW and rollback.
namespace {
std::atomic<void*> platformHandle{nullptr};
std::atomic<bool> platformPause{false}, platformEntered{false}, platformRelease{false};
std::atomic<bool> platformStw{false}, platformAdmission{false}, platformFail{false};
std::atomic<bool> directEntered{false}, directRelease{false};
void HoldDirectFini() {
    directEntered.store(true, std::memory_order_release);
    while (!directRelease.load(std::memory_order_acquire)) { std::this_thread::yield(); }
}
void ArmPlatform(void* handle) {
    platformHandle.store(handle, std::memory_order_release);
    platformEntered.store(false);
    platformRelease.store(false);
    platformPause.store(true, std::memory_order_release);
}
}
extern "C" int dlclose(void* handle) noexcept
{
    using Close = int (*)(void*);
    static Close realClose = reinterpret_cast<Close>(dlsym(RTLD_NEXT, "dlclose"));
    if (realClose == nullptr) { return -1; }
    if (platformFail.exchange(false, std::memory_order_acq_rel)) { return -1; }
    if (platformPause.load(std::memory_order_acquire) && handle == platformHandle.load(std::memory_order_acquire)) {
        platformStw.store(MapleRuntime::MutatorManager::Instance().WorldStopped());
        bool admitted = false;
        std::thread probe([&] { admitted = MapleRuntime::ElfUnloadQuiescenceTest::TrySharedAdmission(); });
        probe.join();
        platformAdmission.store(!admitted);
        platformEntered.store(true, std::memory_order_release);
        while (!platformRelease.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        platformPause.store(false, std::memory_order_release);
    }
    return realClose(handle);
}

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
Metadata secondaryMetadata;
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
CJFile* RegisterMetadata(Metadata& value, const char* name)
{
    value.header.cJFileSize = sizeof(value);
    value.header.tables[GC_FLAGS_TABLE] = { offsetof(Metadata, flags), sizeof(value.flags) };
    value.header.tables[GLOBAL_INIT_FUNC_TABLE] = { offsetof(Metadata, entries), sizeof(value.entries) };
    auto* file = new CJFile(CString(name), reinterpret_cast<Uptr>(&value));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(file);
    loader->RegisterLoadFile(reinterpret_cast<Uptr>(&value));
    return file;
}
void RegisterImage() { image = RegisterMetadata(metadata, "package-init-main"); }
void Init(uint32_t workers = 1)
{
    (void)setenv("cjProcessorNum", workers == 1 ? "1" : "2", 1);
    RuntimeParam param {};
    param.coParam.processorNum = workers;
    param.heapParam.heapSize = 32 * 1024;
    Target("runtime-start", InitCJRuntime(&param) == E_OK);
    Target("configured-scheduler-workers", Runtime::Current().GetConcurrencyModel().GetProcessorNum() == workers);
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
    std::atomic<bool> witnessDone { false }, finish { false }, secondPackageDone { false };
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
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
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

void SharedDependencyPackage(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* owner = nullptr;
    Target("second-package-execute", Begin(reinterpret_cast<const void*>(&PackageB),
        reinterpret_cast<const void*>(&Aggregate), 0, &owner) == Code(Result::Execute));
    void* dependency = nullptr;
    Target("shared-dependency-not-reexecuted", Begin(P(), U(), 0, &dependency) == Code(Result::Ready) &&
           dependency == nullptr && c.payload == 73);
    MCC_PackageInitComplete(owner);
    c.secondPackageDone.store(true, std::memory_order_release);
}

void Repeat(void* p)
{
    auto& c = *static_cast<Completion*>(p);
    void* token = nullptr;
    const uint32_t expected = Code(c.fail || c.abandon ? Result::Failed : Result::Ready);
    Target("sticky-terminal", Begin(P(), U(), 0, &token) == expected && token == nullptr);
    c.witnessDone.store(true, std::memory_order_release);
}
void CompletionCase(bool fail, bool abandon, uint32_t workers = 1)
{
    Init(workers);
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
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    race.witness.store(true, std::memory_order_release);
}
} // namespace

GC_RUNTIME_OTHER_VM_TEST(PackageInit, CompletionWaitsForBodyAndHandshake) { CompletionCase(false, false); }
GC_RUNTIME_OTHER_VM_TEST(PackageInit, CompletionWithTwoSchedulerWorkers) { CompletionCase(false, false, 2); }
GC_RUNTIME_OTHER_VM_TEST(PackageInit, FailureWakesAndRemainsSticky) { CompletionCase(true, false); }
GC_RUNTIME_OTHER_VM_TEST(PackageInit, OwnerExitWakesAndFails) { CompletionCase(false, true); }
GC_RUNTIME_OTHER_VM_TEST(PackageInit, PackagePhaseUnitAndReentry)
{
    Init();
    std::atomic<bool> done { false };
    Start(Isolation, &done);
    Target("isolation-completed", Await(done));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, CrossOwnerCycleHasTerminalResult)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, RegisteredMainCodeAndImageGeneration)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, AggregateWaitsForLastUnit)
{
    Init();
    Completion c;
    c.aggregate = true;
    Start(Owner, &c);
    Target("owner-reached-body", Await(c.ownerStarted));
    Start(AggregateOwner, &c);
    Start(SharedDependencyPackage, &c);
    Start(Waiter, &c);
    Target("waiter-reached-begin", Await(c.waiterStarted));
    Start(Witness, &c);
    Target("aggregate-witness-completed", Await(c.witnessDone));
    c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&c.release, nullptr, nullptr);
    Target("aggregate-completed", Await(c.waiterDone) && c.waiterResult == Code(Result::Ready));
    Target("both-packages-share-completed-dependency", Await(c.secondPackageDone));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&c.release);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, DirectUnloadAllowsOwnerDependency)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, AdmissionCompetitionParksAndRetries)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, ForeignCJThreadWaitsForCompletion)
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
    while (!parked->InSaferegion() &&
           std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    Target("foreign-really-parked", parked->InSaferegion() &&
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, LibraryCodeUnloadReloadHasNewState)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, NativeDlcloseAllowsDependency)
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
GC_RUNTIME_OTHER_VM_TEST(PackageInit, LibInitBodyDoesNotHoldGlobalReader)
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
    while (!ElfUnloadQuiescenceTest::UnloadPending() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Target("unrelated-unload-reached-drain", ElfUnloadQuiescenceTest::UnloadPending());
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
                static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader())->RemoveLoadedFiles(image);
                RegisterImage();
                void* newGeneration = nullptr;
                Target("new-generation-own-token", Begin(P(), U(), 0, &newGeneration) == Code(Result::Execute) &&
                       newGeneration != nullptr && newGeneration != token);
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
GC_RUNTIME_TEST(PackageInit, DuplicateTokenRejected) { CheckTokenMisuse(false); }
GC_RUNTIME_TEST(PackageInit, NonOwnerTokenRejected) { CheckTokenMisuse(true); }
GC_RUNTIME_OTHER_VM_TEST(PackageInit, MultipleMetadataOwnersRequireExactIdentity)
{
    Init();
    // The second metadata owner shares the executable image, with P as its
    // only canonical initializer. Q remains unambiguous in the first owner.
    secondaryMetadata.entries[0] = reinterpret_cast<Uptr>(&PackageA);
    secondaryMetadata.entries[1] = reinterpret_cast<Uptr>(&PackageA);
    auto* secondary = RegisterMetadata(secondaryMetadata, "package-init-secondary");
    std::atomic<bool> done { false };
    Start([](void* argument) {
        void* token = reinterpret_cast<void*>(1);
        Target("duplicate-canonical-owner-rejected", Begin(P(), U(), 0, &token) == Code(Result::Unavailable) && !token);
        Target("ambiguous-image-fallback-rejected", Begin(V(), U(), 0, &token) == Code(Result::Unavailable) && !token);
        Target("exact-owner-disambiguates-image", Begin(reinterpret_cast<const void*>(&PackageB), U(), 0,
            &token) == Code(Result::Execute));
        MCC_PackageInitComplete(token);
        static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
    }, &done);
    Target("multi-owner-completed", Await(done));
    static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader())->RemoveLoadedFiles(secondary);
    Target("remaining-image-still-linked", ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<Uptr>(P())));
    done.store(false, std::memory_order_release);
    Start([](void* argument) {
        void* token = nullptr;
        Target("remaining-owner-state-retained", Begin(reinterpret_cast<const void*>(&PackageB), U(), 0,
            &token) == Code(Result::Ready));
        Target("former-ambiguity-now-resolves", Begin(P(), U(), 0, &token) == Code(Result::Execute));
        MCC_PackageInitComplete(token);
        static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
    }, &done);
    Target("remaining-owner-completed", Await(done));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
namespace {
std::atomic<bool> managedInitBodyRan { false };
void ManagedInitBody()
{
    // This is the same entry-state invariant required by product PinArray.
    Target("libinit-body-has-managed-access", !Mutator::GetMutator()->InSaferegion());
    managedInitBodyRan.store(true, std::memory_order_release);
}
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, LibInitTransitionsNativeCallerToManagedBody)
{
    Init();
    packageBody = ManagedInitBody;
    std::atomic<bool> done { false };
    Start([](void* argument) {
        ScopedEnterSaferegion native(false);
        Target("libinit-native-caller-control", Mutator::GetMutator()->InSaferegion());
        Target("libinit-managed-entry-return", LoaderManager::GetInstance()->LibInit("package-init-main"));
        Target("libinit-restores-native-state", Mutator::GetMutator()->InSaferegion());
        static_cast<std::atomic<bool>*>(argument)->store(true, std::memory_order_release);
    }, &done);
    Target("libinit-body-actually-executed", Await(done) && managedInitBodyRan.load(std::memory_order_acquire));
    packageBody = nullptr;
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
#ifdef MRT_TESTABLE_INTERNALS
namespace {
std::string FixtureBesideExecutable(const char* name)
{
    char executable[4096] {};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    std::string path(executable, static_cast<size_t>(length > 0 ? length : 0));
    return path.substr(0, path.find_last_of('/') + 1) + name;
}
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, DirectUnloadWaitsForPostUnlinkObserver)
{
    Init();
    const std::string path = FixtureBesideExecutable("libcj_package_init_fixture.so");
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    Target("rendezvous-image-open", library != nullptr);
    auto metadata = reinterpret_cast<void* (*)()>(dlsym(library, "PackageInitImageMetadata"));
    auto arm = reinterpret_cast<void (*)(void (*)())>(dlsym(library, "PackageInitImageArmUnload"));
    Target("rendezvous-image-symbols", metadata && arm);
    auto* file = new CJFile(CString("rendezvous-image"), reinterpret_cast<Uptr>(metadata()));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(file);
    loader->RegisterLoadFile(file->GetFileMetaAddr());
    const Uptr imageAddress = file->GetFileMetaAddr();
    arm([]() {});

    std::atomic<bool> ready { false }, activate { false }, active { false }, release { false };
    std::atomic<HandshakeState*> state { nullptr };
    std::thread observer([&]() {
        Target("rendezvous-observer-attach", MRT_NewForeignCJThread());
        auto* mutator = Mutator::GetMutator();
        mutator->SetManagedContext(false);
        mutator->EnterSaferegion(false);
        state.store(&Handshake::Current(), std::memory_order_release);
        ready.store(true, std::memory_order_release);
        while (!activate.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        mutator->LeaveSaferegion();
        active.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        mutator->EnterSaferegion(false);
        Target("rendezvous-observer-detach", MRT_EndForeignCJThread());
    });
    Target("rendezvous-observer-ready", Await(ready));
    auto reader = std::make_unique<ElfUnloadQuiescence::ReadScope>();
    std::atomic<bool> closed { false };
    std::thread closer([&]() {
        Target("rendezvous-platform-close", dlclose(library) == 0);
        closed.store(true, std::memory_order_release);
    });
    const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ElfUnloadQuiescenceTest::UnloadPending() && std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::yield();
    }
    Target("rendezvous-unlink-reached", ElfUnloadQuiescenceTest::UnloadPending());
    // The preflight has finished. Start a managed observer before releasing
    // the real metadata reader, so the post-unlink rendezvous must wait for it.
    activate.store(true, std::memory_order_release);
    Target("rendezvous-observer-active", Await(active));
    reader.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!closed.load(std::memory_order_acquire) &&
           !state.load(std::memory_order_acquire)->has_operation() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool rendezvousPending = state.load(std::memory_order_acquire)->has_operation();
    Target("rendezvous-before-purge", rendezvousPending && !closed.load(std::memory_order_acquire) &&
           ElfUnloadQuiescence::IsAddressInImage(imageAddress, imageAddress));
    release.store(true, std::memory_order_release);
    observer.join();
    Target("rendezvous-close-after-observer", Await(closed));
    closer.join();
    Target("rendezvous-image-purged", !ElfUnloadQuiescence::IsAddressInImage(imageAddress, imageAddress));
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, PublicUnloadDropsStwBeforePlatform)
{
    Init();
    const std::string aPath = FixtureBesideExecutable("libcj_package_init_fixture.so");
    const std::string uPath = FixtureBesideExecutable("libcj_package_init_unrelated.so");
    void* imageA = dlopen(aPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    Target("b0-a-open", imageA != nullptr);
    auto getA = reinterpret_cast<void* (*)()>(dlsym(imageA, "PackageInitImageMetadata"));
    auto armA = reinterpret_cast<void (*)(void (*)())>(dlsym(imageA, "PackageInitImageArmUnload"));
    Target("b0-a-symbols", getA != nullptr && armA != nullptr);
    auto* fileA = new CJFile(CString("package-init-direct-a"), reinterpret_cast<Uptr>(getA()));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(fileA);
    loader->RegisterLoadFile(fileA->GetFileMetaAddr());
    armA([]() {});
    Target("b0-u-load", LoaderManager::GetInstance()->LoadCJLibrary(uPath.c_str()) != nullptr);
    auto getU = reinterpret_cast<void* (*)()>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageMetadata")));
    auto armU = reinterpret_cast<void (*)(void (*)())>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageArmUnload")));
    Target("b0-u-symbols", getU != nullptr && armU != nullptr);
    auto* fileU = new CJFile(CString("libcj_package_init_unrelated.so"), reinterpret_cast<Uptr>(getU()));
    loader->AddLoadedFiles(fileU);
    loader->RegisterLoadFile(fileU->GetFileMetaAddr());
    const Uptr uMeta = fileU->GetFileMetaAddr();
    armU([]() {});
    directEntered.store(false);
    directRelease.store(false);
    armA(HoldDirectFini);
    ArmPlatform(CJFileLoaderTest::Handle(*loader, uPath.c_str()));
    std::atomic<bool> aClosed { false };
    std::atomic<bool> uClosed { false };
    std::thread closerA([&]() {
        Target("b0-a-close", dlclose(imageA) == 0);
        aClosed.store(true, std::memory_order_release);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!directEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    Target("b0-direct-preflight-entered", directEntered.load(std::memory_order_acquire));
    std::thread closerU([&]() {
        Target("b0-u-public-close", UnloadCJLibrary(uPath.c_str()) == E_OK);
        uClosed.store(true, std::memory_order_release);
    });
    while (!platformEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    Target("b0-public-platform-boundary", platformEntered.load(std::memory_order_acquire));
    Target("b0-u-closing", ElfUnloadQuiescence::IsImageClosing(uMeta));
    Target("b0-public-platform-without-stw",
           !platformStw.load() &&
           !platformAdmission.load());
    platformRelease.store(true, std::memory_order_release);
    directRelease.store(true, std::memory_order_release);
    Target("b0-a-completed", Await(aClosed));
    Target("b0-u-completed", Await(uClosed));
    closerA.join();
    closerU.join();
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, PublicUnloadAllowsPendingOwnerAndIdleU)
{
    Init();
    const std::string aPath = FixtureBesideExecutable("libcj_package_init_fixture.so");
    const std::string uPath = FixtureBesideExecutable("libcj_package_init_unrelated.so");
    void* imageA = dlopen(aPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    Target("b1-a-open", imageA != nullptr);
    auto getA = reinterpret_cast<void* (*)()>(dlsym(imageA, "PackageInitImageMetadata"));
    auto armA = reinterpret_cast<void (*)(void (*)())>(dlsym(imageA, "PackageInitImageArmUnload"));
    struct Context {
        Completion c;
        const void* package;
        const void* unit;
        std::atomic<bool> done { false };
        std::atomic<bool> closed { false };
        std::atomic<bool> publicDone { false };
    } context;
    context.package = dlsym(imageA, "PackageInitImagePackage");
    context.unit = dlsym(imageA, "PackageInitImageUnit");
    Target("b1-a-symbols", getA && armA && context.package && context.unit);
    auto* fileA = new CJFile(CString("package-init-direct-a"), reinterpret_cast<Uptr>(getA()));
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(fileA);
    loader->RegisterLoadFile(fileA->GetFileMetaAddr());
    armA(NativeFiniNotice);
    nativeFiniEntered.store(false, std::memory_order_release);
    Target("b1-u-load", LoaderManager::GetInstance()->LoadCJLibrary(uPath.c_str()) != nullptr);
    auto getU = reinterpret_cast<void* (*)()>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageMetadata")));
    auto armU = reinterpret_cast<void (*)(void (*)())>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageArmUnload")));
    Target("b1-u-symbols", getU != nullptr && armU != nullptr);
    auto* fileU = new CJFile(CString("libcj_package_init_unrelated.so"), reinterpret_cast<Uptr>(getU()));
    loader->AddLoadedFiles(fileU);
    loader->RegisterLoadFile(fileU->GetFileMetaAddr());
    armU([]() {});
    Start([](void* argument) {
        auto& c = *static_cast<Context*>(argument);
        void* owner = nullptr;
        Target("b1-owner", Begin(c.package, c.unit, 0, &owner) == Code(Result::Execute));
        c.c.ownerStarted.store(true, std::memory_order_release);
        WaitqueuePark(&c.c.release, LLONG_MAX, Released, &c.c, false);
        void* dependency = nullptr;
        Target("b1-dependency", Begin(P(), V(), 0, &dependency) == Code(Result::Execute));
        MCC_PackageInitComplete(dependency);
        MCC_PackageInitComplete(owner);
        c.done.store(true, std::memory_order_release);
    }, &context);
    Target("b1-owner-started", Await(context.c.ownerStarted));
    ArmPlatform(CJFileLoaderTest::Handle(*loader, uPath.c_str()));
    std::thread closerA([&]() {
        Target("b1-a-close", dlclose(imageA) == 0);
        context.closed.store(true, std::memory_order_release);
    });
    Target("b1-a-fini", Await(nativeFiniEntered));
    std::thread closerU([&]() {
        Target("b1-u-public-close", UnloadCJLibrary(uPath.c_str()) == E_OK);
        context.publicDone.store(true, std::memory_order_release);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!platformEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    Target("b1-public-platform-without-stw",
           platformEntered.load(std::memory_order_acquire) &&
           !platformStw.load());
    platformRelease.store(true, std::memory_order_release);
    context.c.finish.store(true, std::memory_order_release);
    WaitqueueWakeAll(&context.c.release, nullptr, nullptr);
    Target("b1-owner-done", Await(context.done));
    Target("b1-direct-close-done", Await(context.closed));
    Target("b1-public-close-done", Await(context.publicDone));
    closerA.join();
    closerU.join();
    Target("runtime-finish", FiniCJRuntime() == E_OK);
    WaitqueueDelete(&context.c.release);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, DuplicatePublicCloseIsBusy)
{
    Init();
    const std::string uPath = FixtureBesideExecutable("libcj_package_init_unrelated.so");
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    Target("dup-u-load", LoaderManager::GetInstance()->LoadCJLibrary(uPath.c_str()) != nullptr);
    auto getU = reinterpret_cast<void* (*)()>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageMetadata")));
    auto armU = reinterpret_cast<void (*)(void (*)())>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageArmUnload")));
    Target("dup-u-symbols", getU != nullptr && armU != nullptr);
    auto* fileU = new CJFile(CString("libcj_package_init_unrelated.so"), reinterpret_cast<Uptr>(getU()));
    loader->AddLoadedFiles(fileU);
    loader->RegisterLoadFile(fileU->GetFileMetaAddr());
    armU([]() {});
    ArmPlatform(CJFileLoaderTest::Handle(*loader, uPath.c_str()));
    std::atomic<int> firstRc { 1 };
    std::atomic<int> secondRc { 0 };
    std::thread first([&]() {
        firstRc.store(UnloadCJLibrary(uPath.c_str()), std::memory_order_release);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!platformEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    Target("dup-first-reserved", platformEntered.load(std::memory_order_acquire));
    secondRc.store(UnloadCJLibrary(uPath.c_str()), std::memory_order_release);
    Target("dup-second-busy", secondRc.load(std::memory_order_acquire) != E_OK);
    platformRelease.store(true, std::memory_order_release);
    first.join();
    Target("dup-first-ok", firstRc.load(std::memory_order_acquire) == E_OK);
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(PackageInit, PlatformUnloadFailureRollsBack)
{
    Init();
    const std::string uPath = FixtureBesideExecutable("libcj_package_init_unrelated.so");
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    Target("fail-u-load", LoaderManager::GetInstance()->LoadCJLibrary(uPath.c_str()) != nullptr);
    auto getU = reinterpret_cast<void* (*)()>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageMetadata")));
    auto armU = reinterpret_cast<void (*)(void (*)())>(
        reinterpret_cast<void*>(loader->FindSymbol(uPath.c_str(), "PackageInitImageArmUnload")));
    Target("fail-u-symbols", getU != nullptr && armU != nullptr);
    auto* fileU = new CJFile(CString("libcj_package_init_unrelated.so"), reinterpret_cast<Uptr>(getU()));
    loader->AddLoadedFiles(fileU);
    loader->RegisterLoadFile(fileU->GetFileMetaAddr());
    armU([]() {});
    platformFail.store(true, std::memory_order_release);
    Target("fail-injected-close", UnloadCJLibrary(uPath.c_str()) != E_OK);
    Target("fail-handle-retained", CJFileLoaderTest::Handle(*loader, uPath.c_str()) != nullptr);
    Target("fail-retry-close", UnloadCJLibrary(uPath.c_str()) == E_OK);
    Target("fail-handle-cleared", CJFileLoaderTest::Handle(*loader, uPath.c_str()) == nullptr);
    Target("runtime-finish", FiniCJRuntime() == E_OK);
}
GC_TEST(PackageInit, PublicUnloadWithoutRuntimeInitErasesHandler)
{
    const std::string uPath = FixtureBesideExecutable("libcj_package_init_unrelated.so");
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    Target("uninit-load", LoaderManager::GetInstance()->LoadCJLibrary(uPath.c_str()) != nullptr);
    Target("uninit-handle-present", CJFileLoaderTest::Handle(*loader, uPath.c_str()) != nullptr);
    Target("uninit-close", UnloadCJLibrary(uPath.c_str()) == E_OK);
    Target("uninit-handle-erased", CJFileLoaderTest::Handle(*loader, uPath.c_str()) == nullptr);
}
#endif
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
