#include "Heap/Verify/PinFireDiag.h"
namespace MapleRuntime {
namespace PinFireDiag {
bool Enabled() { return false; }
void NoteAddRawPointer() {  }
void NoteCollectPinnedGarbage() {  }
void NoteSkipFreeSlots(RegionInfo* region) {  }
void NoteSkipRegion(RegionInfo* region) {  }
void Report(const char* point) {  }
} // namespace PinFireDiag
} // namespace MapleRuntime
