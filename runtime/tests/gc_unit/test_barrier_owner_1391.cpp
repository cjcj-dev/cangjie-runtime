#include "gc_unittest.hpp"
#include "gc_product_access_test.hpp"
#include "Cangjie.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"
#include "ObjectModel/MObject.h"
#include <thread>
#include <condition_variable>
#include <mutex>

namespace MapleRuntime {
extern "C" BaseObject* MCC_AtomicReadReference(BaseObject* obj, RefField<true>* field, std::memory_order order);
extern "C" ObjRef MCC_NewObject(const TypeInfo* type, MSize size);
}

using namespace MapleRuntime;

namespace {
void InitializeBarrierRuntime()
{
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    params.gcParam.concGCThreadsSet = true;
    params.gcParam.youngGCThreads = 1;
    params.gcParam.youngGCThreadsSet = true;
    params.gcParam.oldGCThreads = 1;
    params.gcParam.oldGCThreadsSet = true;
    params.gcParam.staticGCThreads = true;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, InitializationPublishesOwner)
{
    InitializeBarrierRuntime();
    BarrierSet* owner = ZCollectedHeapTest::BarrierOwner();
    BarrierSet* published = BarrierSet::barrier_set();
    std::fprintf(stderr, "BARRIER1391_IDENTITY_TARGET owner=%p published=%p\n", owner, published);
    GC_EXPECT_TRUE(published == owner);
    GC_EXPECT_EQ(published->kind(), BarrierSet::ZBarrierSetKind);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, NativeCreateBeforeAttach)
{
    InitializeBarrierRuntime();
    bool resourceReady = false;
    uintptr_t before = 0;
    uintptr_t after = 0;
    std::thread native([&] {
        ThreadGCData& data = ThreadLocal::GetNativeGCData();
        resourceReady = data.storeBarrierBuffer != nullptr;
        before = data.storeGoodMask;
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        after = data.storeGoodMask;
    });
    native.join();
    std::fprintf(stderr, "BARRIER1391_CREATE_TARGET resource=%d mask_before=%lx mask_after=%lx\n",
                 resourceReady, before, after);
    GC_EXPECT_TRUE(resourceReady && before == 0 && after == ZPointerStoreGoodMask);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, NativeOwnerAttachesPublishedMasks)
{
    InitializeBarrierRuntime();
    bool resourceReady = false;
    uintptr_t loadBad = 0;
    uintptr_t storeGood = 0;
    std::thread native([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        const ThreadGCData& data = ThreadLocal::GetGCData();
        resourceReady = data.storeBarrierBuffer != nullptr;
        loadBad = data.loadBadMask;
        storeGood = data.storeGoodMask;
    });
    native.join();
    std::fprintf(stderr, "BARRIER1391_NATIVE_TARGET resource=%d load_bad=%lx store_good=%lx\n",
                 resourceReady, loadBad, storeGood);
    GC_EXPECT_EQ(loadBad, ZPointerLoadBadMask);
    GC_EXPECT_EQ(storeGood, ZPointerStoreGoodMask);
    GC_EXPECT_TRUE(resourceReady);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, ManagedOwnerAttachesPublishedMasks)
{
    InitializeBarrierRuntime();
    struct Observation {
        bool resourceReady = false;
        uintptr_t loadBad = 0;
        uintptr_t storeGood = 0;
        BaseObject* allocated = nullptr;
    } observation;
    auto task = RunCJTask([](void* input) -> void* {
        auto& output = *static_cast<Observation*>(input);
        const ThreadGCData& data = Mutator::GetMutator()->GetGCData();
        output.resourceReady = data.storeBarrierBuffer != nullptr;
        output.loadBad = data.loadBadMask;
        output.storeGood = data.storeGoodMask;
        alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
        auto* type = reinterpret_cast<TypeInfo*>(storage);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(24);
        type->SetAlign(8);
        GCTib tib{};
        tib.tag = SIGN_BIT;
        type->SetGCTib(tib);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
        output.allocated = static_cast<BaseObject*>(MCC_NewObject(type, 32));
        return nullptr;
    }, &observation);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "BARRIER1391_MANAGED_TARGET resource=%p load_bad=%lx store_good=%lx\n",
                 reinterpret_cast<void*>(observation.resourceReady), observation.loadBad, observation.storeGood);
    GC_EXPECT_EQ(observation.loadBad, ZPointerLoadBadMask);
    GC_EXPECT_EQ(observation.storeGood, ZPointerStoreGoodMask);
    GC_EXPECT_TRUE(observation.resourceReady);
    GC_EXPECT_TRUE(observation.allocated != nullptr);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, CreateDoesNotResetWatermark)
{
    InitializeBarrierRuntime();
    Mutator mutator;
    const ZStackWatermark& watermark = mutator.GetStackWatermark();
    const ThreadGCData& data = mutator.GetGCData();
    std::fprintf(stderr, "BARRIER1391_WATERMARK_TARGET head=%lx epoch=%lx buffer=%p owner=%p\n",
                 watermark.prev_head_color(), watermark.GetEpoch(), data.storeBarrierBuffer,
                 BarrierSet::barrier_set());
    GC_EXPECT_TRUE(data.storeBarrierBuffer != nullptr);
    GC_EXPECT_EQ(watermark.prev_head_color(), static_cast<uintptr_t>(ZPointerStoreBadMask));
    GC_EXPECT_EQ(watermark.watermark(), static_cast<uintptr_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, DestroyNullsPairedBuffer)
{
    InitializeBarrierRuntime();
    Mutator mutator;
    ThreadGCData& data = mutator.GetGCData();
    StoreBarrierBuffer* created = data.storeBarrierBuffer;
    BarrierSet* published = BarrierSet::barrier_set();
    std::fprintf(stderr, "BARRIER1391_DESTROY_TARGET created=%p published=%p\n", created, published);
    GC_EXPECT_TRUE(created != nullptr);
    GC_EXPECT_TRUE(published != nullptr);
    published->on_thread_destroy(data);
    std::fprintf(stderr, "BARRIER1391_DESTROY_AFTER buffer=%p\n", data.storeBarrierBuffer);
    GC_EXPECT_TRUE(data.storeBarrierBuffer == nullptr);
    published->on_thread_create(data);
    GC_EXPECT_TRUE(data.storeBarrierBuffer != nullptr);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, CompilerAtomicReadsPublishedBackend)
{
    InitializeBarrierRuntime();
    alignas(uintptr_t) uintptr_t storage = static_cast<uintptr_t>(ZPointerStoreGoodMask);
    auto* field = reinterpret_cast<RefField<true>*>(&storage);
    BaseObject* result = MCC_AtomicReadReference(nullptr, field, std::memory_order_seq_cst);
    std::fprintf(stderr, "BARRIER1391_ACCESS_TARGET result=%p stored=%lx\n", result, storage);
    GC_EXPECT_TRUE(result == nullptr);
    GC_EXPECT_EQ(storage, static_cast<uintptr_t>(ZPointerStoreGoodMask));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// Observe the cleaner through thread exit, including the stopped-runtime arm.
// Resource release and hook identities are checked externally at product entry
// and return addresses; the test never calls a lifecycle hook itself.
GC_RUNTIME_OTHER_VM_TEST(BarrierOwner1391, NativeExitAfterRuntimeStop)
{
    InitializeBarrierRuntime();
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    bool release = false;
    bool resourceReady = false;
    std::thread native([&] {
        ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
        resourceReady = ThreadLocal::GetGCData().storeBarrierBuffer != nullptr;
        std::unique_lock<std::mutex> lock(mutex);
        ready = true;
        condition.notify_one();
        condition.wait(lock, [&] { return release; });
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready; });
    }
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_one();
    native.join();
    std::fprintf(stderr, "BARRIER1391_STOPPED_EXIT_TARGET resource=%d joined=1\n", resourceReady);
    GC_EXPECT_TRUE(resourceReady);
}
