#include "Heap/Verify/GarbRegionDiag.h"
namespace MapleRuntime {
namespace GarbRegionDiag {
bool Enabled() { return false; }
void CensusBeforeForward(const char* where) {  }
void NoteCollectEnter(RegionInfo* region) {  }
void NoteF3Join(RegionInfo* latestRegion, BaseObject* latest, const char* reason) {  }
void Report(const char* point) {  }
} // namespace GarbRegionDiag
} // namespace MapleRuntime
