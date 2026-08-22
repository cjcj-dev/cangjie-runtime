// Static String.myData: seal-before-evacuate cannot remap; after relocate the
// plain RootSlot must name the to-copy and read back the original payload.

#include <cstring>

#include "Heap/Collector/Collector.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class TableCollector final : public Collector {
public:
    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    BaseObject* FindToVersion(BaseObject* obj) const override { return obj == from ? to : nullptr; }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
};

static void WritePayload(BaseObject* obj, uint64_t v)
{
    *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(obj) + TYPEINFO_PTR_SIZE) = v;
}

static uint64_t ReadPayload(BaseObject* obj)
{
    return *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(obj) + TYPEINFO_PTR_SIZE);
}

} // namespace

GC_TEST(StaticHeal, SealBeforeRelocateFindToMisses)
{
    GcHeapFixture fx;
    TableCollector c;
    c.from = fx.obj0;
    c.to = nullptr;
    GC_EXPECT_TRUE(c.FindToVersion(fx.obj0) == nullptr);
}

GC_TEST(StaticHeal, AfterRelocatePlainSlotReadsOriginalPayload)
{
    GcHeapFixture fx;
    constexpr uint64_t kPayload = 0x5347474e5f535452ULL;
    WritePayload(fx.obj0, kPayload);
    std::memcpy(static_cast<void*>(fx.obj1), static_cast<const void*>(fx.obj0), 64);

    TableCollector c;
    c.from = fx.obj0;
    c.to = fx.obj1;

    MAddress slot = reinterpret_cast<MAddress>(fx.obj0);
    BaseObject* via = c.FindToVersion(reinterpret_cast<BaseObject*>(slot));
    GC_EXPECT_TRUE(via == fx.obj1);
    slot = reinterpret_cast<MAddress>(via);
    GC_EXPECT_EQ(ReadPayload(reinterpret_cast<BaseObject*>(slot)), kPayload);
    GC_EXPECT_TRUE(reinterpret_cast<BaseObject*>(slot) != fx.obj0);
}
