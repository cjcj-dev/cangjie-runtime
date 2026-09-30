#include <atomic>
#include <cstdio>
#include <thread>

#include "Cangjie.h"
#include "CompilerCalls.h"
#include "TypeInfoManager.h"
#include "gc_unittest.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zAddress.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void CheckBindingAfterCollection(bool managerBinding, bool managedEntry = false, bool collect = true)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
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
        if (managedEntry) {
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
                 "BINDING_MASK_TARGET executed=1 manager=%d managed=%d before=%#zx after=%#zx global=%#zx pending=%d active=%d\n",
                 managerBinding, managedEntry, before, after, expected, pending, active);
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
