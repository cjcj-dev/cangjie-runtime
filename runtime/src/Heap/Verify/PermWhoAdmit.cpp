#include "Heap/Verify/PermWhoAdmit.h"
namespace MapleRuntime {
namespace PermWhoAdmit {
bool Enabled() { return false; }
void NoteRoute(RegionInfo* region, BaseObject* from, BaseObject* to) {  }
void NoteRoutePlan(RegionInfo* region, size_t fromBytes, unsigned densifyOutcome) {  }
void NoteAbandon(RegionInfo* region, size_t walkedObjects, size_t forwardedObjects) {  }
void DumpSummary() {  }
} // namespace PermWhoAdmit
} // namespace MapleRuntime
