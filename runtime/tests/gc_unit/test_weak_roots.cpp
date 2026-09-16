#include "Common/SuspendibleThreadSet.h"
#include "Common/WeakHandle.inline.h"
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct RestoreBlock {
    CollectorResources& resources;
    ~RestoreBlock() { resources.UnblockResurrection(); }
};

zpointer* SlotOf(NativeSlot& slot)
{
    return reinterpret_cast<zpointer*>(&slot);
}
}

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
    GC_EXPECT_TRUE(handle.ptr_raw() == nullptr);
    std::fprintf(stderr, "WEAK_HANDLE_EMPTY_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, PhantomCleanDeadClearsSlot)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RestoreBlock restore{resources};
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    resources.BlockResurrection();
    GC_EXPECT_TRUE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(is_null_any(*SlotOf(slot)));
    std::fprintf(stderr, "WEAK_ROOTS_DEAD_CLEAN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, PhantomCleanLiveRetainsSlot)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RestoreBlock restore{resources};
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    (void)GcHeapFixture::MarkStrong(fx.region0, fx.obj0);
    resources.BlockResurrection();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_LIVE_RETAIN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, PhantomCleanFinalizableRetainsSlot)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RestoreBlock restore{resources};
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    (void)GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0);
    resources.BlockResurrection();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_FINALIZABLE_RETAIN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakHandleProduct, PeekBlockedOldDoesNotKeepDead)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RestoreBlock restore{resources};
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    resources.BlockResurrection();
    GC_EXPECT_TRUE(NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(&slot) == nullptr);
    GC_EXPECT_TRUE(NativeAccess<ON_PHANTOM_OOP_REF>::oop_load(&slot) == nullptr);
    std::fprintf(stderr, "WEAK_HANDLE_PEEK_DEAD_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, YoungBlockedAccessDoesNotDeathClean)
{
    GcHeapFixture fx;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RestoreBlock restore{resources};
    fx.region0->reset(PageAge::eden);
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, true, false);
    resources.BlockResurrection();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_YOUNG_NO_DEATH_CLEAN_ASSERT_EXECUTED\n");
}
