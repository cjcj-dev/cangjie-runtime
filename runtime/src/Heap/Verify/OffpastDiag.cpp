#include "Heap/Verify/OffpastDiag.h"
namespace MapleRuntime {
namespace OffpastDiag {
bool Enabled() { return false; }
void NotePregrant(BaseObject* obj, const char* site) {  }
void NotePregrantSlot(void* slot, BaseObject* obj, const char* site) {  }
void NoteRouteEnter(RegionInfo* region) {  }
void NoteCompactDone(RegionInfo* region) {  }
void NoteFixMiss(BaseObject* obj) {  }
void NoteFixMissSlot(void* slot, BaseObject* obj) {  }
} // namespace OffpastDiag
} // namespace MapleRuntime
