#include "Heap/Collector/FinalizerProcessor.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(FnlzRoots, VisitGCRootsSeesRegisteredFinalizers)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char storage[16] = {};
    auto* obj = reinterpret_cast<BaseObject*>(storage);
    fp.RegisterFinalizer(obj);
    size_t n = 0;
    fp.VisitGCRoots([&](RootSlot&) { ++n; });
    GC_EXPECT_EQ(n, static_cast<size_t>(1));
}

GC_TEST(FnlzRoots, VisitFinalizersCountMatchesRegister)
{
    FinalizerProcessor fp;
    alignas(8) unsigned char a[16] = {};
    alignas(8) unsigned char b[16] = {};
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(a));
    fp.RegisterFinalizer(reinterpret_cast<BaseObject*>(b));
    U32 n = fp.VisitFinalizers([](RootSlot&) {});
    GC_EXPECT_EQ(n, static_cast<U32>(2));
}
