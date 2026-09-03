#include <cstring>
#include <csignal>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"

#include "Common/ColourPredicates.h"
#include "Heap/Barrier/Barrier.h"
#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_WriteGenericPayload(MapleRuntime::ObjectPtr dst, MapleRuntime::MAddress srcField,
                                           size_t srcSize);
extern "C" void CJ_MCC_ReadGenericPayload(void* dstNative, MapleRuntime::ObjectPtr obj, size_t size);

namespace {

class NoAnswerCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override { return FindToVersionResult::NotForwarded(); }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(object, remap));
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override { return object; }
};

class InstalledBarrierScope {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }
    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
};

struct PayloadFixture {
    PayloadFixture() : barrier(collector, rememberedSet), installed(barrier)
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        auto& heapRemset = Heap::GetHeap().GetRememberedSet();
        if (!heapRemset.initialized) {
            heapRemset.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        }
        heap.typeInfo->SetFlag(0);
        heap.typeInfo->SetInstanceSize(sizeof(uint64_t));
    }

    GcHeapFixture heap;
    NoAnswerCollector collector;
    RememberedSet rememberedSet;
    IdleBarrier barrier;
    InstalledBarrierScope installed;
};

void ExpectControlledAbort(const std::function<void()>& body)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        body();
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

} // namespace

GC_TEST(PayloadClamp, ReadAtLimitCopies)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    const uint64_t expected = 0x1122334455667788ULL;
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.heap.obj1) + TYPEINFO_PTR_SIZE),
                &expected, sizeof(expected));
    uint64_t got = 0;
    CJ_MCC_ReadGenericPayload(&got, fx.heap.obj1, limit);
    GC_EXPECT_EQ(got, expected);
}

GC_TEST(PayloadClamp, WriteAtLimitCopies)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    const uint64_t expected = 0xaabbccddeeff0011ULL;
    CJ_MCC_WriteGenericPayload(fx.heap.obj1, reinterpret_cast<MAddress>(&expected), limit);
    uint64_t got = 0;
    std::memcpy(&got, reinterpret_cast<void*>(reinterpret_cast<MAddress>(fx.heap.obj1) + TYPEINFO_PTR_SIZE),
                sizeof(got));
    GC_EXPECT_EQ(got, expected);
}

GC_OTHER_VM_TEST(PayloadClamp, ReadOverLimitAborts)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    uint64_t buf[2] = {};
    ExpectControlledAbort([&]() { CJ_MCC_ReadGenericPayload(buf, fx.heap.obj1, limit + 1); });
}

GC_OTHER_VM_TEST(PayloadClamp, WriteOverLimitAborts)
{
    PayloadFixture fx;
    const size_t limit = fx.heap.typeInfo->GetInstanceSize();
    uint64_t src[2] = { 1, 2 };
    ExpectControlledAbort([&]() {
        CJ_MCC_WriteGenericPayload(fx.heap.obj1, reinterpret_cast<MAddress>(src), limit + 1);
    });
}
