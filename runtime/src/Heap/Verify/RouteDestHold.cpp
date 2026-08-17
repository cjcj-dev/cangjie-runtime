#include "Heap/Verify/RouteDestHold.h"
namespace MapleRuntime {
namespace RouteDestHold {
bool AccountOn() { return false; }
bool InjectHandbackOn() { return false; }
bool HoldsBack(const RegionInfo* region, Site site) { return false; }
void NoteReuse(const RegionInfo* region, bool held) {  }
void NoteReclaimFunnel(const RegionInfo* region, const char* site) {  }
void NoteClearPoint(size_t heldRegions, size_t heldBytes) {  }
void NoteTo2Resolve(uintptr_t arith, uint32_t idx) {  }
void DumpSummary() {  }
} // namespace RouteDestHold
} // namespace MapleRuntime
