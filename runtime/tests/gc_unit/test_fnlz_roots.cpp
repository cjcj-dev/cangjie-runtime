#include "Heap/Collector/FinalizerProcessor.h"
#include "gc_unittest.hpp"

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
