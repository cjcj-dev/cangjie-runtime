#include "gc_unittest.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Mutator/Mutator.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct CountingIterator {
    size_t* visits;
    explicit CountingIterator(size_t* visits) : visits(visits) {}
    void Apply(int&) { ++*visits; }
};
}

GC_TEST(P10Roots, ParallelApplyCompletesOnce)
{
    size_t visits = 0;
    ParallelApply<CountingIterator> apply(&visits);
    int token = 0;
    apply.apply(&token);
    apply.apply(&token);
    GC_EXPECT_EQ(visits, size_t(1));
    GC_EXPECT_TRUE(apply.CompletedForTest());
    std::fprintf(stderr, "P10_PARALLEL_APPLY_ASSERT_EXECUTED visits=%zu\n", visits);
}

GC_TEST(P10Roots, HeapIteratorBitMapTrySetOnce)
{
    HeapIteratorBitMap bits(64);
    GC_EXPECT_TRUE(bits.try_set_bit(3));
    GC_EXPECT_FALSE(bits.try_set_bit(3));
    GC_EXPECT_TRUE(bits.try_set_bit(4));
    std::fprintf(stderr, "P10_BITMAP_ASSERT_EXECUTED\n");
}

GC_TEST(P10Roots, PackedWatermarkState)
{
    StackWatermark watermark;
    GC_EXPECT_TRUE(watermark.TryBegin(11, StackWatermark::WM_OWNER_SELF, 0));
    GC_EXPECT_EQ(StackWatermark::UnpackEpoch(watermark.PackedState()), 11u);
    GC_EXPECT_FALSE(StackWatermark::UnpackDone(watermark.PackedState()));
    watermark.Finish(StackWatermark::WM_OWNER_SELF);
    GC_EXPECT_TRUE(StackWatermark::UnpackDone(watermark.PackedState()));
    std::fprintf(stderr, "P10_WATERMARK_PACK_ASSERT_EXECUTED state=%u\n", watermark.PackedState());
}

GC_TEST(P10Roots, HandleMarkPopsNativeRoots)
{
    Mutator mutator;
    {
        HandleMark mark(mutator);
        (void)mutator.AddNativeFrameRoot(nullptr);
        GC_EXPECT_EQ(mutator.NativeFrameRootCount(), size_t(1));
    }
    GC_EXPECT_EQ(mutator.NativeFrameRootCount(), size_t(0));
    std::fprintf(stderr, "P10_HANDLE_MARK_ASSERT_EXECUTED\n");
}
