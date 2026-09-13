// ZGC zServiceability.cpp:41-53,142-150; no dedicated upstream gtest.
#include "gc_unittest.hpp"
#include "Heap/z/zServiceability.hpp"
#include "Heap/z/zGCIdPrinter.hpp"
using namespace MapleRuntime;

GC_TEST(ZServiceability, GenerationCapacityAndLimit)
{
    const auto info = ComputeMemoryUsageInfo(100, 200, 30, 40);
    GC_EXPECT_EQ(info.young.used, 30U);
    GC_EXPECT_EQ(info.young.current, 60U);
    GC_EXPECT_EQ(info.young.max, 200U);
    GC_EXPECT_EQ(info.old.used, 40U);
    GC_EXPECT_EQ(info.old.current, 40U);
    GC_EXPECT_EQ(info.old.max, 200U);
}

GC_TEST(ZServiceability, ConcurrentSamplesClampToCapacity)
{
    const auto info = ComputeMemoryUsageInfo(100, 200, 90, 70);
    GC_EXPECT_EQ(info.young.used, 30U);
    GC_EXPECT_EQ(info.old.used, 70U);
    const auto exceeded = ComputeMemoryUsageInfo(100, 200, 90, 110);
    GC_EXPECT_EQ(exceeded.young.current, 0U);
    GC_EXPECT_EQ(exceeded.old.current, 100U);
}

GC_TEST(ZServiceability, CapacityBeyondSigned32Bits)
{
    const size_t capacity = size_t{1} << 33;
    const size_t maximum = size_t{1} << 34;
    const auto info = ComputeMemoryUsageInfo(capacity, maximum, capacity / 4, capacity / 2);
    GC_EXPECT_EQ(info.young.current + info.old.current, capacity);
    GC_EXPECT_EQ(info.young.max, maximum);
    GC_EXPECT_EQ(info.old.max, maximum);
}

GC_TEST(ZGCIdPrinter, MinorAndMajorCoexist)
{
    GCIdMark major;
    const auto majorId = GCIdMark::Current();
    {
        ZGCIdMajor young(majorId, 'Y');
        GC_EXPECT_EQ(ZGCIdPrinter::Tag(majorId), 'Y');
        bool minorMatches = false;
        std::thread minor([majorId, &minorMatches] {
            GCIdMark id;
            ZGCIdMinor tag(GCIdMark::Current());
            minorMatches = GCIdMark::Current() != majorId &&
                ZGCIdPrinter::Tag(GCIdMark::Current()) == 'y' && ZGCIdPrinter::Tag(majorId) == 'Y';
        });
        minor.join();
        GC_EXPECT_TRUE(minorMatches);
        GC_EXPECT_EQ(GCIdMark::Current(), majorId);
    }
    {
        ZGCIdMajor old(majorId, 'O');
        GC_EXPECT_EQ(ZGCIdPrinter::Tag(majorId), 'O');
    }
}

GC_TEST(ZGCIdPrinter, WorkerScopeRestoresThreadContext)
{
    GCIdMark submitting;
    const auto id = GCIdMark::Current();
    bool workerMatches = false;
    std::thread worker([id, &workerMatches] {
        workerMatches = GCIdMark::Current() == 0;
        {
            GCIdMark task(id);
            workerMatches = workerMatches && GCIdMark::Current() == id;
        }
        workerMatches = workerMatches && GCIdMark::Current() == 0;
    });
    worker.join();
    GC_EXPECT_TRUE(workerMatches);
}
