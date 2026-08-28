#include "Mutator/Mutator.h"
#include "UnwindStack/EhStackInfo.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/StackExposureHook.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(StackExposureProduct, MutatorProcessUsesProcessOneNotAdvanceOnly)
{
    Mutator* mutator = Mutator::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    mutator->SetManagedContext(true);
    StackWatermark& wm = mutator->GetStackWatermark();
    wm.Reset();
    GC_EXPECT_TRUE(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 8));
    StackExposureHook::ResetStats();
    const bool fired = StackExposureHook::OnBeforeUnwind(*mutator, 0);
    GC_EXPECT_TRUE(fired);
    GC_EXPECT_TRUE(StackExposureHook::FireCount() >= 1);
    wm.Reset();
    mutator->SetManagedContext(false);
}

GC_TEST(StackExposureProduct, EhFillInStackTraceFiresOnIteration)
{
    Mutator* mutator = Mutator::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    StackWatermark& wm = mutator->GetStackWatermark();
    wm.Reset();
    GC_EXPECT_TRUE(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 64));
    StackExposureHook::ResetStats();
    EHStackInfo eh;
    eh.FillInStackTrace();
    GC_EXPECT_TRUE(StackExposureHook::FireCount() >= 1);
    wm.Reset();
}

GC_TEST(StackExposureProduct, GcFillInStackTraceFiresOnIteration)
{
    Mutator* mutator = Mutator::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    StackWatermark& wm = mutator->GetStackWatermark();
    wm.Reset();
    GC_EXPECT_TRUE(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 64));
    StackExposureHook::ResetStats();
    GCStackInfo gc;
    gc.FillInStackTrace();
    GC_EXPECT_TRUE(StackExposureHook::FireCount() >= 1);
    wm.Reset();
}

GC_TEST(StackExposureProduct, BoundCollectorVisitorIsInvoked)
{
    Mutator* mutator = Mutator::GetMutator();
    GC_EXPECT_TRUE(mutator != nullptr);
    mutator->SetManagedContext(true);
    StackWatermark& wm = mutator->GetStackWatermark();
    wm.Reset();
    GC_EXPECT_TRUE(wm.TryBegin(1, StackWatermark::WM_OWNER_SELF, 8));
    size_t boundHits = 0;
    RootVisitor bound = [&boundHits](ObjectRef&) { ++boundHits; };
    DerivedPtrVisitor derived = [](BasePtrType, DerivedSlot&) {};
    mutator->BindExposureVisitors(&bound, &derived);
    StackExposureHook::ResetStats();
    const bool fired = StackExposureHook::OnBeforeUnwind(*mutator, 0);
    GC_EXPECT_TRUE(fired);
    GC_EXPECT_TRUE(StackExposureHook::ProcessOneCount() >= 1);
    mutator->UnbindExposureVisitors();
    wm.Reset();
    mutator->SetManagedContext(false);
}
