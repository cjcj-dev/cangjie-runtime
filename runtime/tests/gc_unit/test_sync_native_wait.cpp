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
    NativeWaitSet* n = new NativeWaitSet();
    n->busy.store(1);
    n->object = WeakHandle(&SyncWeakOopStorage(), fx.obj0);
    SyncRetireDead();
    GC_EXPECT_TRUE(n->object.resolve() == fx.obj0);
    n->busy.store(0);
    n->object.release(&SyncWeakOopStorage());
    delete n;
    std::fprintf(stderr, "SYNC_RETIRE_BUSY_ASSERT_EXECUTED\n");
}
