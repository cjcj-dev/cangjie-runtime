// Request source for the managed fixture, not an implementation of collection.
// ZGC zCollectedHeap.cpp:174-181: route an external young request to the minor driver.
#include "Heap/z/zCollectedHeap.hpp"

extern "C" void PhaseEntryRequestMinor()
{
    MapleRuntime::ZCollectedHeap::heap()->collect(MapleRuntime::GC_REASON_YOUNG);
}
