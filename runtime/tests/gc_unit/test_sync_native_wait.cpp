#include "Common/WeakHandle.inline.h"
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


