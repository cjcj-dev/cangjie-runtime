#include "Common/SuspendibleThreadSet.h"
#include "Common/WeakHandle.inline.h"
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(WeakRootsProduct, EmptyRendezvousCompletes)
{
    ZRendezvousGCThreads op;
    op.doit();
    GC_EXPECT_FALSE(SuspendibleThreadSet::should_yield());
    std::fprintf(stderr, "WEAK_ROOTS_EMPTY_RENDEZVOUS_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, JoinerLeaveWakesSynchronize)
{
    std::atomic<bool> joined{false};
    std::atomic<bool> done{false};
    std::thread participant([&] {
        SuspendibleThreadSetJoiner joiner;
        joined.store(true, std::memory_order_release);
        while (!SuspendibleThreadSet::should_yield()) {
            std::this_thread::yield();
        }
        joiner.yield();
        done.store(true, std::memory_order_release);
    });
    while (!joined.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    ZRendezvousGCThreads op;
    op.doit();
    participant.join();
    GC_EXPECT_TRUE(done.load(std::memory_order_acquire));
    std::fprintf(stderr, "WEAK_ROOTS_JOIN_YIELD_ASSERT_EXECUTED\n");
}

GC_TEST(WeakHandleProduct, EmptyHandleIsNull)
{
    WeakHandle handle;
    GC_EXPECT_TRUE(handle.is_null());
    GC_EXPECT_TRUE(handle.is_empty());
    GC_EXPECT_EQ(handle.ptr_raw(), static_cast<NativeSlot*>(nullptr));
    std::fprintf(stderr, "WEAK_HANDLE_EMPTY_ASSERT_EXECUTED\n");
}
