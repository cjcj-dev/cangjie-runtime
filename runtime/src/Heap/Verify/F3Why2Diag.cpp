#include "Heap/Verify/F3Why2Diag.h"
namespace MapleRuntime {
namespace F3Why2Diag {
bool Enabled() { return false; }
void NoteCollectEnter(RegionInfo* region) {  }
void NoteF3RegionGarbage(RegionInfo* latestRegion, BaseObject* latest) {  }
void NoteForwardOrder(RegionInfo* region, uint64_t liveBefore, size_t markedBefore, uint64_t liveAfterReset, size_t markedAfterReset, size_t markedAfterInvalidate) {  }
void Report(const char* point) {  }
void CountMarks(RegionInfo* region, size_t& validOut, size_t& markedOut) {  }
} // namespace F3Why2Diag
} // namespace MapleRuntime
