#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"
#include "gc_heap_fixture.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zMark.hpp"
#include "Common/MarkWorkStack.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
int WaitChild(pid_t child)
{
    int childStatus = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &childStatus, 0);
    } while (waited < 0 && errno == EINTR);
    GC_EXPECT_EQ(waited, child);
    return childStatus;
}

void PrepareYoungMark(GcHeapFixture& heap, uint32_t workers)
{
    heap.region0->reset(PageAge::eden);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    auto& young = Heap::GetHeap().young();
    young.InitializeWorkers(workers);
    young.Mark().Start();
    young.PublishPhase(ZGenerationPhase::Mark);
}
}

GC_TEST(MarkEntry805, ValidOopPushYoungMarks)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    PrepareYoungMark(heap, 1);
    HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj0) + TYPEINFO_PTR_SIZE).StoreColoured(zpointer::null);
    WorkStack work;
    std::vector<BaseObject*> reached;
    std::unordered_set<MAddress> slots;
    std::unordered_set<MAddress> weak;
    ZMark::PushYoungObject(heap.obj0, work, "mark-entry-805");
    ZMark::TraceYoungClosure(work, false, reached, slots, weak);
    const bool marked = heap.region0->is_object_strongly_live(from_object(heap.obj0));
    Heap::GetHeap().young().StopWorkers();
    std::fprintf(stderr, "MARK_ENTRY805_VALID marked=%d\n", marked);
    GC_EXPECT_TRUE(marked);
}

GC_TEST(MarkEntry805, NullKlassPushYoungFailsValidObject)
{
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        B09RuntimeFixture runtime;
        GcHeapFixture heap;
        PrepareYoungMark(heap, 1);
        std::memset(heap.obj0, 0, sizeof(void*));
        WorkStack work;
        ZMark::PushYoungObject(heap.obj0, work, "mark-entry-805-null-klass");
        std::fprintf(stderr, "MARK_ENTRY805_NULL_KLASS_SURVIVED\n");
        Heap::GetHeap().young().StopWorkers();
        _exit(4);
    }
    const int childStatus = WaitChild(child);
    std::fprintf(stderr, "MARK_ENTRY805_NULL_KLASS status=%d exited=%d code=%d signaled=%d\n",
                 childStatus, WIFEXITED(childStatus), WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1,
                 WIFSIGNALED(childStatus));
    const bool survived = WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 4;
    GC_EXPECT_FALSE(survived);
}
