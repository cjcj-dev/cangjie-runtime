#include "Heap/z/zRootsIterator.hpp"
#include "Heap/z/zReferenceProcessor.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zWorkers.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_worker_fixture.hpp"
#include "gc_unittest.hpp"
#include "Mutator/Mutator.h"

#include <algorithm>
#include <array>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Heap/z/zAccess.hpp"

namespace MapleRuntime {
extern "C" U64 CJ_MCC_CreateExportHandle(BaseObject*);
extern "C" void CJ_MCC_RemoveExportedRef(U64);
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_OTHER_VM_TEST(FnlzRoots, ExportBlockGrowthKeepsSlotsAndReleaseSkipsVacancies)
{
    auto& heap = Heap::GetHeap();
    GcHeapFixture fixture;
    std::array<BaseObject*, 130> objects;
    for (size_t i = 0; i < objects.size(); ++i) {
        objects[i] = fixture.PlaceObject(fixture.heapStart + 128 + i * 16);
    }
    fixture.region0()->SetRegionAllocPtr(fixture.heapStart + 128 + objects.size() * 16);
    std::vector<U64> handles;
    NativeSlot* first = nullptr;
    for (size_t index = 0; index < 130; ++index) {
        handles.push_back(CJ_MCC_CreateExportHandle(objects[index]));
        if (index == 0) {
            heap.cross_vm().export_roots().VisitGCRoots([&](NativeSlot& slot) {
                if (to_object(slot.GetTargetObject()) == objects[0]) { first = &slot; }
            });
        }
    }
    size_t seen = 0;
    NativeSlot* grown = nullptr;
    heap.cross_vm().export_roots().VisitGCRoots([&](NativeSlot& slot) {
        for (auto& object : objects) {
            if (to_object(slot.GetTargetObject()) == object) {
                ++seen;
                if (&object == &objects[0]) { grown = &slot; }
            }
        }
    });
    std::fprintf(stderr, "ROOT_STORAGE_TARGET export_registered=%zu first=%p grown=%p\n",
                 seen, static_cast<void*>(first), static_cast<void*>(grown));
    GC_EXPECT_EQ(seen, size_t(130));
    GC_EXPECT_TRUE(first != nullptr && first == grown);
    for (U64 handle : handles) { CJ_MCC_RemoveExportedRef(handle); }
    size_t releasedSeen = 0;
    heap.cross_vm().export_roots().VisitGCRoots([&](NativeSlot& slot) {
        for (auto& object : objects) {
            if (to_object(slot.GetTargetObject()) == object) { ++releasedSeen; }
        }
    });
    std::fprintf(stderr, "ROOT_STORAGE_TARGET export_released_remaining=%zu\n", releasedSeen);
    GC_EXPECT_EQ(releasedSeen, size_t(0));
}

// universe.cpp:619-626 and java_lang_ref_Reference pending-list exchange:
// one strong native root holds the managed pending chain, not one per referent.
GC_OTHER_VM_TEST(FnlzRoots, PendingTransferClearsPersistentRoot)
{
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    GcHeapFixture fixture;
    auto& processor = Heap::GetHeap().GetFinalizerProcessor();
    GC_EXPECT_TRUE(processor.SwapPendingList(fixture.obj0) == nullptr);
    NativeSlot* pending = nullptr;
    processor.VisitGCRoots([&](NativeSlot& slot) { pending = &slot; });
    GC_EXPECT_TRUE(pending != nullptr);
    BaseObject* transferred = processor.WaitPending();
    const zpointer cleared = pending->GetFieldValue();
    const bool same = transferred == fixture.obj0;
    const bool released = is_null_any(cleared) && ZPointer::is_mark_good(cleared);
    std::fprintf(stderr, "REFERENCE1356_PENDING transfer=%d cleared_store_good=%d slots=%zu\n",
                 same, released, processor.StrongRootStorage().AllocationCount());
    GC_EXPECT_TRUE(same && released);
    GC_EXPECT_EQ(processor.StrongRootStorage().AllocationCount(), size_t(1));
}
