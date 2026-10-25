#define MRT_USE_CJTHREAD_RENAME 1
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

#include "Cangjie.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zAddress.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* type, MSize size);
extern "C" ObjectPtr CJ_MCC_ReadRefField(ObjectPtr object, RefField<false>* field);
}

namespace {
void CheckBindingAfterCollection(bool managerBinding, bool managedEntry = false, bool collect = true,
                                 bool resumedEntry = false)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    using Resume = void (*)(Mutator*, ThreadLocalData*);
    auto resume = reinterpret_cast<Resume>(dlsym(RTLD_DEFAULT,
        "_ZN12MapleRuntime7Mutator13PreparedToRunEPNS_15ThreadLocalDataE"));
    GC_EXPECT_TRUE(!resumedEntry || resume != nullptr);
    std::atomic<bool> ready{false};
    std::atomic<bool> collected{false};
    uintptr_t before = 0;
    uintptr_t after = 0;
    uintptr_t expected = 0;
    bool pending = true;
    bool active = false;
    bool readNull = false;
    std::thread thread([&] {
        Mutator* owner = Mutator::NewMutator();
        owner->SetManagedContext(false);
        before = owner->GetGCData().storeGoodMask;
        ready.store(true, std::memory_order_release);
        while (!collected.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        auto* tls = ThreadLocal::GetThreadLocalData();
        auto& manager = MutatorManager::Instance();
        if (managerBinding) {
            manager.BindMutator(*owner);
        } else {
            tls->SetMutator(owner);
        }
        pending = HasPendingSafepoint(tls);
        if (resumedEntry) {
            resume(owner, tls);
        } else if (managedEntry) {
            MRT_PreRunManagedCode(owner, 0, tls);
        } else {
            MRT_LeaveSaferegion();
        }
        after = tls->gcData->storeGoodMask;
        expected = ::g_cjStoreGoodMask;
        active = !owner->InSaferegion();
        owner->SetManagedContext(false);
        alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(32 - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
        auto* object = reinterpret_cast<BaseObject*>(MCC_NewObject(type, 32));
        auto* field = reinterpret_cast<RefField<false>*>(reinterpret_cast<uintptr_t>(object) + TYPEINFO_PTR_SIZE);
        readNull = CJ_MCC_ReadRefField(object, field) == nullptr;
        MRT_EnterSaferegion(false);
        manager.UnbindMutator(*owner);
        manager.UnregisterMarkFlushThread(tls);
        delete owner;
    });
    while (!ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    if (collect) {
        Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    }
    collected.store(true, std::memory_order_release);
    thread.join();
    std::fprintf(stderr,
                 "BINDING_MASK_TARGET executed=1 manager=%d managed=%d resumed=%d before=%#zx after=%#zx global=%#zx pending=%d active=%d\n",
                 managerBinding, managedEntry, resumedEntry, before, after, expected, pending, active);
    const bool target = after == expected;
    const bool setup = (collect ? before != expected : before == expected) && !pending && active;
    GC_EXPECT_TRUE(target);
    GC_EXPECT_TRUE(setup);
    GC_EXPECT_TRUE(readNull);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(BindingPoll, ManagerBindingAfterCollection)
{
    CheckBindingAfterCollection(true);
}

GC_RUNTIME_OTHER_VM_TEST(BindingPoll, CarrierBindingAfterCollection)
{
    CheckBindingAfterCollection(false);
}

GC_RUNTIME_OTHER_VM_TEST(BindingPoll, ManagedEntryAfterCollection)
{
    CheckBindingAfterCollection(true, true);
}

GC_RUNTIME_OTHER_VM_TEST(BindingPoll, FreshOwnerWithoutCollection)
{
    CheckBindingAfterCollection(true, false, false);
}

GC_RUNTIME_OTHER_VM_TEST(BindingPoll, ResumedEntryAfterCollection)
{
    CheckBindingAfterCollection(false, false, true, true);
}

#if defined(__linux__) && defined(__x86_64__)
#include <limits.h>
#include <unistd.h>
#include "Interpreter/RuntimeAPI.h"
#include "Mutator/ThreadSMR.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "b09_runtime_fixture.hpp"
#include "schedule.h"
namespace {
void* LoadFiniteInterpreter()
{
    char path[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    GC_EXPECT_TRUE(n > 0);
    path[n] = 0;
    const std::string library = std::string(path).substr(0, std::string(path).find_last_of('/') + 1) +
                                "libcj_interpreter_root_fixture.so";
    InterpreterParam param{};
    param.interpreterLibName = library.c_str();
    GC_EXPECT_EQ(InitCJInterpreter(&param), E_OK);
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_NOLOAD);
    GC_EXPECT_TRUE(handle != nullptr);
    return handle;
}
struct AttachObservation {
    std::thread::id creator = std::this_thread::get_id();
    Mutator* owner = nullptr;
    bool phaseAdmitted = false;
    uintptr_t before = 0;
    uintptr_t after = 0;
};
void FlipDuringInterpreterStart(void* context, void* owner)
{
    auto& observation = *static_cast<AttachObservation*>(context);
    if (std::this_thread::get_id() != observation.creator) { return; }
    observation.owner = static_cast<Mutator*>(owner);
    observation.before = ThreadGCData::PublishedMasks().storeGood;
    // Nonblocking probe: never await a phase flip while the producer owns its
    // correct lock. An illegal lock across the external callback fails directly.
    std::thread phase([&] {
        auto& manager = MutatorManager::Instance();
        observation.phaseAdmitted = manager.TryAcquireMutatorManagementWLock();
        if (observation.phaseAdmitted) {
            ZGlobalsPointers::flip_old_relocate_start();
            manager.MutatorManagementWUnlock();
        }
    });
    phase.join();
    observation.after = ThreadGCData::PublishedMasks().storeGood;
}
void* UnstartedTask(void*, unsigned int) { return nullptr; }
}
GC_RUNTIME_OTHER_VM_TEST(LogicalAttach, PublishesMasksAndEpochAfterInterpreterConstruction)
{
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 512 * 1024;
    parameters.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&parameters), E_OK);
    void* library = LoadFiniteInterpreter();
    using SetHook = void (*)(void (*)(void*, void*), void*);
    auto setHook = reinterpret_cast<SetHook>(dlsym(library, "GCInterpreterSetStartHook"));
    GC_EXPECT_TRUE(setHook != nullptr);
    AttachObservation observed;
    setHook(FlipDuringInterpreterStart, &observed);
    CJThreadAttr attr{};
    CJThreadAttrInit(&attr);
    CJThreadAttrStackSizeSet(&attr, 1024 * 1024);
    void* task = CJThreadNew(Runtime::Current().GetConcurrencyModel().GetThreadScheduler(), &attr,
                            UnstartedTask, nullptr, 0, CJTHREAD_CREATE_SOURCE_DEFAULT, 0);
    setHook(nullptr, nullptr);
    std::fprintf(stderr, "LOGICAL_ATTACH_SETUP task=%p owner=%p\n", task, observed.owner);
    GC_EXPECT_TRUE(task != nullptr);
    GC_EXPECT_TRUE(observed.owner != nullptr);
    ThreadsListHandle members;
    const auto& data = observed.owner->GetGCData();
    const auto epoch = observed.owner->GetStackWatermark().GetEpoch();
    const bool published = members.includes(observed.owner);
    std::fprintf(stderr, "LOGICAL_ATTACH_TARGET executed=1 phase_admitted=%d before=%#zx after=%#zx "
                         "attached=%#zx epoch=%#zx current_epoch=%#zx buffer=%#zx published=%d\n",
                 observed.phaseAdmitted, observed.before, observed.after, data.storeGoodMask,
                 static_cast<size_t>(epoch), static_cast<size_t>(StackWatermark::epoch_id()),
                 data.storeBarrierBuffer->lastProcessedColor, published);
    GC_EXPECT_TRUE(observed.phaseAdmitted);
    GC_EXPECT_NE(observed.before, observed.after);
    GC_EXPECT_TRUE(published);
    GC_EXPECT_EQ(data.storeGoodMask, observed.after);
    GC_EXPECT_EQ(epoch, StackWatermark::epoch_id());
    GC_EXPECT_EQ(data.storeBarrierBuffer->lastProcessedColor, observed.after);
}

namespace {
size_t interpreterRoots = 0;
size_t interpreterHeadRoots = 0;
uintptr_t* interpreterSlots[2]{};
uintptr_t interpreterOldColor = 0;
void ObserveInterpreterRoot(zaddress_unsafe* slot, uintptr_t color)
{
    GC_EXPECT_EQ(color, interpreterOldColor);
    auto* location = reinterpret_cast<uintptr_t*>(slot);
    if (location == interpreterSlots[0] || location == interpreterSlots[1]) {
        ++interpreterRoots;
        *location += 0x40;
    } else {
        // The head's exception root is visited even when it contains null.
        ++interpreterHeadRoots;
        GC_EXPECT_EQ(*location, uintptr_t{0});
    }
}
void CheckInterpreterWatermark(bool interpreted)
{
    B09RuntimeFixture runtime(8);
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    void* library = LoadFiniteInterpreter();
    void* pc = interpreted ? dlsym(library, "GCInterpreterFramePC") :
                             reinterpret_cast<void*>(&UnstartedTask);
    GC_EXPECT_TRUE(pc != nullptr);
    // Input frame chain is classified by the product from the loaded interpreter
    // image. Its finite ABI consumer reads roots from each received frame.fp.
    alignas(16) uintptr_t frames[2][4]{};
    frames[0][0] = reinterpret_cast<uintptr_t>(&frames[1][0]);
    frames[0][1] = reinterpret_cast<uintptr_t>(pc);
    frames[0][2] = 0x1000;
    frames[1][2] = 0x2000;
    Mutator owner;
    owner.SetManagedContext(true);
    owner.GetUnwindContext().frameInfo = FrameInfo {
        MachineFrame(reinterpret_cast<FrameAddress*>(&frames[0][0]), reinterpret_cast<uint32_t*>(pc)),
        FrameType::RUNTIME };
    owner.GetUnwindContext().frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(&frames[0][0]));
    owner.GetStackWatermark().Reset();
    interpreterOldColor = owner.GetStackWatermark().GetEpoch();
    ZGlobalsPointers::flip_old_relocate_start();
    interpreterRoots = 0;
    interpreterHeadRoots = 0;
    interpreterSlots[0] = &frames[0][2];
    interpreterSlots[1] = &frames[1][2];
    StackWatermarkSet::finish_processing(owner, reinterpret_cast<void*>(&ObserveInterpreterRoot));
    std::fprintf(stderr, "INTERPRETER_WATERMARK_TARGET executed=1 interpreted=%d roots=%zu "
                         "first=%#zx second=%#zx done=%d\n", interpreted, interpreterRoots,
                 frames[0][2], frames[1][2], owner.GetStackWatermark().IsDone());
    GC_EXPECT_EQ(interpreterHeadRoots, 1u);
    GC_EXPECT_EQ(interpreterRoots, interpreted ? 2u : 0u);
    GC_EXPECT_EQ(frames[0][2], interpreted ? 0x1040u : 0x1000u);
    GC_EXPECT_EQ(frames[1][2], interpreted ? 0x2040u : 0x2000u);
    GC_EXPECT_TRUE(owner.GetStackWatermark().IsDone());
}
}
GC_COMPONENT_OTHER_VM_TEST(InterpreterWatermark, VisitsLoadedInterpreterFrames)
{
    CheckInterpreterWatermark(true);
}
GC_COMPONENT_OTHER_VM_TEST(InterpreterWatermark, NativeFrameControl)
{
    CheckInterpreterWatermark(false);
}
#endif
