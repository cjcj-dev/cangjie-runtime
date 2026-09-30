#include "Heap/shared/barrierSet.hpp"
#include "Base/Log.h"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
BarrierSet* BarrierSet::_barrier_set = nullptr;

void BarrierSet::set_barrier_set(BarrierSet* barrier_set)
{
    assert(_barrier_set == nullptr && "Already initialized");
    ThreadGCData& bootstrap = ThreadLocal::GetNativeGCData();
    _barrier_set = barrier_set;
    _barrier_set->on_thread_create(bootstrap);
}
}
