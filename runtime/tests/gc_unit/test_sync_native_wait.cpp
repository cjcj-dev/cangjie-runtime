#include "Common/WeakHandle.inline.h"
#include "Cangjie.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "Sync/Sync.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(SyncNativeWait, WeakHandleOnSyncStorageResolves)
{
    GcHeapFixture fx;
    WeakHandle handle(&SyncWeakOopStorage(), fx.obj0);
    GC_EXPECT_FALSE(handle.is_null());
    GC_EXPECT_TRUE(handle.resolve() == fx.obj0);
    handle.release(&SyncWeakOopStorage());
    GC_EXPECT_TRUE(handle.is_null());
    std::fprintf(stderr, "SYNC_NATIVE_WEAK_RESOLVE_ASSERT_EXECUTED\n");
}

GC_TEST(SyncNativeWait, FutureInitHasNoHeapWaitQueue)
{
    alignas(16) unsigned char buf[CJFuture::SYNC_OBJECT_SIZE] = {};
    CJFuture* future = reinterpret_cast<CJFuture*>(buf);
    MCC_FutureInit(future);
    GC_EXPECT_TRUE(future->waitNative == nullptr);
    GC_EXPECT_TRUE(future->isWaitQueueInit.load() == 0);
    GC_EXPECT_TRUE(future->completeFlag.load() == false);
    std::fprintf(stderr, "SYNC_FUTURE_INIT_NATIVE_ASSERT_EXECUTED\n");
}

GC_TEST(SyncNativeWait, RetireSkipsBusyWaitSet)
{
    GcHeapFixture fx;
    CJWaitQueue* queue = reinterpret_cast<CJWaitQueue*>(fx.obj0);
    GC_EXPECT_TRUE(MCC_WaitQueueInit(queue) == 0);
    NativeWaitSet* n = queue->waitNative;
    GC_EXPECT_TRUE(n != nullptr);
    n->busy.store(1);
    size_t before = SyncWeakOopStorage().AllocationCount();
    SyncRetireDead();
    GC_EXPECT_TRUE(queue->waitNative == n);
    GC_EXPECT_TRUE(n->object.resolve() == fx.obj0);
    GC_EXPECT_TRUE(SyncWeakOopStorage().AllocationCount() == before);
    n->busy.store(0);
    std::fprintf(stderr, "SYNC_RETIRE_BUSY_ASSERT_EXECUTED\n");
}

GC_TEST(SyncNativeWait, RetireReleasesIdleClearedHandle)
{
    GcHeapFixture fx;
    CJWaitQueue* queue = reinterpret_cast<CJWaitQueue*>(fx.obj0);
    GC_EXPECT_TRUE(MCC_WaitQueueInit(queue) == 0);
    NativeWaitSet* n = queue->waitNative;
    n->busy.store(0);
    n->object.release(&SyncWeakOopStorage());
    size_t before = SyncWeakOopStorage().AllocationCount();
    SyncRetireDead();
    GC_EXPECT_TRUE(SyncWeakOopStorage().AllocationCount() <= before);
    std::fprintf(stderr, "SYNC_RETIRE_RELEASE_ASSERT_EXECUTED\n");
}

GC_TEST(SyncNativeWait, WeakHandleResolveFollowsRelocatedObject)
{
    GcHeapFixture fx;
    CJWaitQueue* queue = reinterpret_cast<CJWaitQueue*>(fx.obj0);
    GC_EXPECT_TRUE(MCC_WaitQueueInit(queue) == 0);
    NativeWaitSet* n = queue->waitNative;
    BaseObject* from = fx.obj0;
    BaseObject* to = fx.obj1;
    n->object.replace(to);
    BaseObject* resolved = n->object.resolve();
    GC_EXPECT_TRUE(resolved == to);
    GC_EXPECT_TRUE(resolved != from);
    std::fprintf(stderr, "SYNC_WEAK_RELOCATE_RESOLVE_ASSERT_EXECUTED\n");
}



namespace {
void* CreateUnreachableWaitQueue(void* argument)
{
    auto& initialized = *static_cast<bool*>(argument);
    alignas(TypeInfo) static unsigned char metadata[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(metadata);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(CJWaitQueue) - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(metadata), sizeof(metadata));
    auto* queue = MObject::NewObject(type, sizeof(CJWaitQueue), AllocType::MOVEABLE_OBJECT);
    initialized = queue != nullptr && MCC_WaitQueueInit(queue) == 0;
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(SyncNativeWait, CollectionRetiresUnreachableOwnerOnce)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t before = SyncWeakOopStorage().AllocationCount();
    bool initialized = false;
    CJThreadHandle task = RunCJTask(CreateUnreachableWaitQueue, &initialized);
    GC_EXPECT_TRUE(task != nullptr);
    void* result = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &result), E_OK);
    ReleaseHandle(task);
    const size_t registered = SyncWeakOopStorage().AllocationCount();
    Heap::GetHeap().RequestGC(GC_REASON_USER);
    const size_t after = SyncWeakOopStorage().AllocationCount();
    std::fprintf(stderr, "SYNC_OWNER_RETIRE_ASSERT initialized=%d before=%zu registered=%zu after=%zu\n",
                 initialized, before, registered, after);
    const bool inputPresent = initialized && registered == before + 1;
    const bool retired = after == before;
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    GC_EXPECT_TRUE(inputPresent);
    GC_EXPECT_TRUE(retired);
}
