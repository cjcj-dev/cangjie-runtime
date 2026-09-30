#include <atomic>
#include <cstdio>
#include <thread>

#include "Cangjie.h"
#include "gc_unittest.hpp"
#include "Heap/Heap.h"
#include "Heap/z/zAddress.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void CheckBindingAfterCollection(bool managerBinding)
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
        MRT_LeaveSaferegion();
        after = tls->gcData->storeGoodMask;
        expected = ::g_cjStoreGoodMask;
        active = !owner->InSaferegion();
        MRT_EnterSaferegion(false);
        manager.UnbindMutator(*owner);
        manager.UnregisterMarkFlushThread(tls);
        delete owner;
    });
    while (!ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    collected.store(true, std::memory_order_release);
    thread.join();
    std::fprintf(stderr,
                 "BINDING_MASK_TARGET executed=1 manager=%d before=%#zx after=%#zx global=%#zx pending=%d active=%d\n",
                 managerBinding, before, after, expected, pending, active);
    const bool target = after == expected;
    const bool setup = before != expected && !pending && active;
    GC_EXPECT_TRUE(target);
    GC_EXPECT_TRUE(setup);
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
