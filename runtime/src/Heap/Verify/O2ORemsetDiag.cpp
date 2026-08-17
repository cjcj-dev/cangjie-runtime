#include "Heap/Verify/O2ORemsetDiag.h"
namespace MapleRuntime {
namespace O2ORemsetDiag {
bool Enabled() { return false; }
void NoteOldObjectForward(BaseObject* fromObj, BaseObject* toObj, size_t size) {  }
void NoteYoungObjectForward() {  }
void NoteRecordedOnTo(size_t n) {  }
void NoteOldRegionForwarded(RegionInfo* region, size_t remsetInFrom, size_t liveObjectsForwarded, size_t o2yEdgesOnToObj, size_t recordedOnTo) {  }
void NoteScrubNonYoung(RegionInfo* region, size_t scrubbed) {  }
void NoteCompactCall(unsigned overload, bool youngRegion) {  }
void NoteCompactObjectMove(BaseObject* fromObj, BaseObject* toObj, size_t size, bool youngRegion) {  }
void NoteCompactRemsetInFrom(size_t n) {  }
void NoteCompactRecordedOnTo(size_t n) {  }
void DumpAndMaybeReset(const char* point, bool reset) {  }
} // namespace O2ORemsetDiag
} // namespace MapleRuntime
