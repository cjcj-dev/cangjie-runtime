#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/z/zStoreBarrierBuffer.hpp"
#undef private

#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_WriteGeneric(const MapleRuntime::ObjectPtr obj, void* fieldPtr,
                                    const MapleRuntime::ObjectPtr src, size_t size);
extern "C" void CJ_MCC_WriteGenericPayload(MapleRuntime::ObjectPtr dst, MapleRuntime::MAddress srcField,
                                           size_t srcSize);

namespace {

zpointer StoreBadPointer(BaseObject* object)
{
    return to_zpointer(raw(StoreGoodPointer(object)) ^ ZPointerMarkedOldMask);
}

class AllocBufferScope final {
public:
    explicit AllocBufferScope(AllocBuffer& alloc) : alloc(alloc), saved(ThreadLocal::GetThreadLocalData()->buffer)
    {
        ThreadLocal::GetThreadLocalData()->buffer = &alloc;
    }
    ~AllocBufferScope()
    {
        ThreadLocal::GetThreadLocalData()->buffer = saved;
        alloc.ClearRegion();
    }

private:
    AllocBuffer& alloc;
    AllocBuffer* saved;
};

class InstalledMutatorScope final {
public:
    explicit InstalledMutatorScope(Mutator& mutator) : saved(ThreadLocal::GetMutator())
    {
        ThreadLocal::SetMutator(&mutator);
    }
    ~InstalledMutatorScope() { ThreadLocal::SetMutator(saved); }

private:
    Mutator* saved;
};

} // namespace

GC_TEST(WriteGeneric, CopiesRefSlotThroughStoreBarrier)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);

    HeapSlot<>& srcField = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    HeapSlot<>& dstField = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    srcField.StoreColoured(StoreGoodPointer(fx.obj1));
    dstField.StoreColoured(StoreBadPointer(fx.obj0));

    CJ_MCC_WriteGeneric(fx.obj0, &dstField, fx.obj1, sizeof(void*));

    GC_EXPECT_TRUE(to_object(dstField.GetTargetObject()) == fx.obj1);
    StoreBarrierBuffer* buf = ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_TRUE(buf != nullptr);
    GC_EXPECT_EQ(buf->Pending(), 1u);
    if (buf->Pending() == 1u) {
        GC_EXPECT_EQ(reinterpret_cast<MAddress>(buf->buffer[buf->Current()].p), reinterpret_cast<MAddress>(&dstField));
    }
    std::fprintf(stderr, "TARGET_WRITE_GENERIC_STORE_BARRIER_EXECUTED pending=%u\n",
                 buf != nullptr ? buf->Pending() : 0u);
}

GC_TEST(WriteGeneric, PayloadSharesWriteStructStoreBarrier)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);

    HeapSlot<>& dstField = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    dstField.StoreColoured(StoreBadPointer(fx.obj0));
    BaseObject* srcWord = fx.obj1;
    CJ_MCC_WriteGenericPayload(fx.obj0, reinterpret_cast<MAddress>(&srcWord), sizeof(void*));

    GC_EXPECT_TRUE(to_object(dstField.GetTargetObject()) == fx.obj1);
    StoreBarrierBuffer* buf = ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_TRUE(buf != nullptr);
    GC_EXPECT_EQ(buf->Pending(), 1u);
    std::fprintf(stderr, "TARGET_WRITE_GENERIC_PAYLOAD_STORE_BARRIER_EXECUTED pending=%u\n",
                 buf != nullptr ? buf->Pending() : 0u);
}
