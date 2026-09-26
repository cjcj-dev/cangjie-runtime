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
    // The second application must not revisit: the public result verifies completion.
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
    const uint32_t pending = StackWatermark::PackState(11, false);
    GC_EXPECT_EQ(StackWatermark::UnpackEpoch(pending), 11u);
    GC_EXPECT_FALSE(StackWatermark::UnpackDone(pending));
    const uint32_t completed = StackWatermark::PackState(11, true);
    GC_EXPECT_TRUE(StackWatermark::UnpackDone(completed));
    GC_EXPECT_EQ(StackWatermark::UnpackEpoch(completed), 11u);
    std::fprintf(stderr, "P10_WATERMARK_PACK_ASSERT_EXECUTED state=%u\n", completed);
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
