// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <dlfcn.h>
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Collector/MarkStripe.h"
#include "Heap/WCollector/WCollector.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
// Bind the existing product implementation, never instantiate a second copy
// of the mark claim in the test ELF. The runtime entry arms are separate from
// these focused accounting checks.
using ProductMark = bool (*)(const WCollector*, BaseObject*, bool, MarkLiveCache*);
ProductMark CachedMark()
{
    auto fn = reinterpret_cast<ProductMark>(dlsym(RTLD_DEFAULT,
        "_ZNK12MapleRuntime10WCollector14MarkObjectImplEPNS_10BaseObjectEbPNS_13MarkLiveCacheE"));
    GC_EXPECT_TRUE(fn != nullptr);
    return fn;
}

void CheckCachedClaim(bool finalizable, bool repeat)
{
    GcHeapFixture fx;
    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    (void)fx.PlantMarkBitmap(live, fx.region0->GetRegionSize());
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    const size_t size = fx.obj0->GetSize();
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    if (finalizable) {
        GC_EXPECT_FALSE(collector.ResurrectObject(fx.obj0, offset, fx.region0));
    }
    MarkLiveCache cache(1);
    const bool already = CachedMark()(&collector, fx.obj0, false, &cache);
    const bool secondAlready = repeat ? CachedMark()(&collector, fx.obj0, false, &cache) : true;
    cache.Flush();
    const uint64_t bytes = fx.region0->GetLiveByteCount();
    // Capture product results before releasing the fixture's bitmap, and do
    // not place a transition assertion ahead of the accounting invariant.
    const bool strong = fx.region0->IsMarkedObject(fx.region0->GetMarkView<Generation::Old>(), fx.obj0);
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
    std::fprintf(stderr, "M2_LIVE_RESULT finalizable=%d repeat=%d bytes=%zu expected=%zu strong=%d\n",
                 finalizable, repeat, static_cast<size_t>(bytes), size, strong);
    GC_EXPECT_EQ(bytes, static_cast<uint64_t>(size));
    GC_EXPECT_TRUE(strong);
    GC_EXPECT_FALSE(already);
    GC_EXPECT_TRUE(secondAlready);
}
}

GC_TEST(MarkPort203Entries, FirstLiveIsAccountedAfterCacheFlush)
{
    CheckCachedClaim(false, false);
}

GC_TEST(MarkPort203Entries, RepeatedStrongClaimDoesNotAccountTwice)
{
    CheckCachedClaim(false, true);
}

GC_TEST(MarkPort203Entries, FinalizableUpgradeDoesNotAccountTwice)
{
    CheckCachedClaim(true, false);
}
