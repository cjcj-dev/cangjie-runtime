#include "Heap/Collector/FinalizerProcessor.h"
#include "gc_unittest.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(FnlzRoots, RegisteredFinalizerIsRawPointerButNotStrongRoot)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char storage[16] = {};
    auto* obj = reinterpret_cast<BaseObject*>(storage);
    fp.RegisterFinalizer(obj);

    size_t strongRoots = 0;
    fp.VisitGCRoots([&](RootSlot&) { ++strongRoots; });
    GC_EXPECT_EQ(strongRoots, static_cast<size_t>(0));

    size_t rawPointers = 0;
    fp.VisitRawPointers([&](RootSlot&) { ++rawPointers; });
    GC_EXPECT_EQ(rawPointers, static_cast<size_t>(1));
}

GC_TEST(FnlzRoots, VisitFinalizersCountMatchesRegister)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char a[16] = {};
    alignas(8) unsigned char b[16] = {};
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(a));
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(b));

    U32 finalizers = fp.VisitFinalizers([](RootSlot&) {});
    GC_EXPECT_EQ(finalizers, static_cast<U32>(2));
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(FnlzRoots, EnqueueBetweenIdleCheckAndCommitKeepsJobVisible)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char storage[16] = {};
    auto* obj = reinterpret_cast<BaseObject*>(storage);
    std::mutex lock;
    std::condition_variable condition;
    bool workerAtOldEmptyToClearGap = false;
    bool releaseWorker = false;

    fp.SetBeforeFinalizableIdleCheckForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        workerAtOldEmptyToClearGap = true;
        condition.notify_one();
        condition.wait(guard, [&] { return releaseWorker; });
    });

    std::thread worker([&] { fp.FinishFinalizableBatchForTest(); });
    {
        std::unique_lock<std::mutex> guard(lock);
        condition.wait(guard, [&] { return workerAtOldEmptyToClearGap; });
    }

    // This is the review's exact old :303 -> enqueue -> :304 ordering.
    fp.EnqueueFinalizableForTest(obj);
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseWorker = true;
    }
    condition.notify_one();
    worker.join();

    GC_EXPECT_TRUE(fp.HasFinalizableJobForTest());
    size_t queuedRoots = 0;
    fp.VisitGCRoots([&](RootSlot&) { ++queuedRoots; });
    GC_EXPECT_EQ(queuedRoots, static_cast<size_t>(1));
}
#endif
