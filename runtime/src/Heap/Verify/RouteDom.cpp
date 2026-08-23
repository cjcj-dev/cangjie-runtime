#include "Heap/Verify/RouteDom.h"
namespace MapleRuntime {
namespace RouteDom {
bool Enabled() { return false; }
void NoteRoute(RegionInfo* region, BaseObject* fromObj, uint64_t preLiveBytes, uintptr_t toAddr) {  }
void DumpSummary() {  }
} // namespace RouteDom
} // namespace MapleRuntime
