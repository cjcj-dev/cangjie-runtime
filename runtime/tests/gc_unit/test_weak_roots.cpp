#include "Common/SuspendibleThreadSet.h"
#include "Cangjie.h"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Common/WeakHandle.inline.h"
#include "Heap/z/zAccess.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct RestoreBlock {
    ~RestoreBlock() { ZResurrection::unblock(); }
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
    RestoreBlock restore;
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    ZResurrection::block();
    GC_EXPECT_TRUE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(is_null_any(*SlotOf(slot)));
    std::fprintf(stderr, "WEAK_ROOTS_DEAD_CLEAN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, PhantomCleanLiveRetainsSlot)
{
    GcHeapFixture fx;
    RestoreBlock restore;
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    (void)GcHeapFixture::MarkStrong(fx.region0(), fx.obj0);
    ZResurrection::block();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_LIVE_RETAIN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, PhantomCleanFinalizableRetainsSlot)
{
    GcHeapFixture fx;
    RestoreBlock restore;
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    (void)GcHeapFixture::MarkFinalizable(fx.region0(), fx.obj0);
    ZResurrection::block();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_FINALIZABLE_RETAIN_ASSERT_EXECUTED\n");
}

GC_TEST(WeakHandleProduct, PeekBlockedOldDoesNotKeepDead)
{
    GcHeapFixture fx;
    RestoreBlock restore;
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    ZResurrection::block();
    GC_EXPECT_TRUE(NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(&slot) == nullptr);
    GC_EXPECT_TRUE(NativeAccess<ON_PHANTOM_OOP_REF>::oop_load(&slot) == nullptr);
    std::fprintf(stderr, "WEAK_HANDLE_PEEK_DEAD_ASSERT_EXECUTED\n");
}

GC_TEST(WeakRootsProduct, YoungBlockedAccessDoesNotDeathClean)
{
    GcHeapFixture fx;
    RestoreBlock restore;
    fx.region0()->reset(PageAge::eden);
    RestoreMarkFlips flips;
    NativeSlot slot(zpointer::null);
    *SlotOf(slot) = CaptureStoreGoodThenFlipMark(fx.obj0, flips, true, false);
    ZResurrection::block();
    GC_EXPECT_FALSE(ZBarrier::clean_barrier_on_phantom_oop_field(SlotOf(slot)));
    GC_EXPECT_TRUE(to_object(ZPointer::uncolor(*SlotOf(slot))) == fx.obj0);
    std::fprintf(stderr, "WEAK_ROOTS_YOUNG_NO_DEATH_CLEAN_ASSERT_EXECUTED\n");
}

// oopStorageSetParState.inline.hpp:76-91: run a real driver collection,
// observing the record retirement performed by the product storage owner.
// The companion debugger check counts the real owner callback, without a
// replacement callback or a product test hook.
GC_RUNTIME_OTHER_VM_TEST(WeakRootsProduct, CollectionReportsDeadToOwnerOnce)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    auto& owner = Heap::GetHeap().GetFinalizerProcessor();
    auto& storage = owner.WeakRootStorage();
    const size_t before = storage.AllocationCount();
    owner.RegisterFinalizer(nullptr);
    const size_t registered = storage.AllocationCount();
    Heap::GetHeap().RequestGC(GC_REASON_USER);
    const size_t after = storage.AllocationCount();
    std::fprintf(stderr, "WEAK_OWNER_RETIRE_ASSERT before=%zu registered=%zu after=%zu\n",
                 before, registered, after);
    const bool inputPresent = registered == before + 1;
    const bool retired = after == before;
    std::fprintf(stderr, "WEAK_OWNER_INPUT_ASSERT result=%d; WEAK_OWNER_RETIRE_ASSERT result=%d\n",
                 inputPresent, retired);
    GC_EXPECT_TRUE(inputPresent);
    GC_EXPECT_TRUE(retired);
}
