// Request source for the managed fixture, not an implementation of collection.
// ZGC zDriver.cpp:133-160/187-190: submit a minor request to the minor driver.
#include "HeapManager.inline.h"

extern "C" void PhaseEntryRequestMinor()
{
    MapleRuntime::HeapManager::RequestGC(MapleRuntime::GC_REASON_YOUNG);
}
