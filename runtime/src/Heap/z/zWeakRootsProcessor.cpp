#include "Heap/z/zWeakRootsProcessor.hpp"
#include "Sync/Sync.h"
#include "Common/SuspendibleThreadSet.h"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/z/zTask.hpp"
#include "Heap/z/zWorkers.hpp"

namespace MapleRuntime {
class ZPhantomCleanOopClosure {
public:
    void do_oop(NativeSlot* p)
    {
        ZBarrier::clean_barrier_on_phantom_oop_field(reinterpret_cast<volatile zpointer*>(p));
        SuspendibleThreadSet::yield();
    }
};

ZWeakRootsProcessor::ZWeakRootsProcessor(ZWorkers* workers) : workers(workers) {}

class ZProcessWeakRootsTask : public ZTask {
private:
    RootsIteratorWeakColored rootsWeakColored;
public:
    explicit ZProcessWeakRootsTask(const Collector& collector, unsigned nworkers)
        : ZTask("ZProcessWeakRootsTask"),
          rootsWeakColored(collector, nworkers, ZGenerationIdOptional::old) {}

    ~ZProcessWeakRootsTask() override
    {
        rootsWeakColored.report_num_dead();
    }

    void work() override
    {
        SuspendibleThreadSetJoiner stsJoiner;
        ZPhantomCleanOopClosure cl;
        rootsWeakColored.Apply([&](NativeSlot& slot) { cl.do_oop(&slot); });
    }
};

void ZWeakRootsProcessor::process_weak_roots()
{
    auto& collector = static_cast<Collector&>(Heap::GetHeap().GetCollector());
    ZProcessWeakRootsTask task(collector, workers->active_workers());
    workers->run(&task);
    SyncRetireDead();
}
} // namespace MapleRuntime
