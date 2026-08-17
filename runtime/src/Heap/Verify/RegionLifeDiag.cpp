#include "Heap/Verify/RegionLifeDiag.h"
namespace MapleRuntime {
namespace RegionLifeDiag {
bool Enabled() { return false; }
bool FreeTrackOn() { return false; }
void NoteTake(RegionInfo* region) {  }
void NoteFree(RegionInfo* region, uint16_t path) {  }
void NoteRelease(RegionInfo* region, uint16_t path) {  }
void SetNextFreePath(uint16_t path) {  }
uint16_t TakeNextFreePath() { return 0; }
bool LookupLastFree(uintptr_t addr, uint32_t* freeSeq, uint32_t* lifeId, uint16_t* path, uint16_t* phase, uint32_t* gcCount, uintptr_t* start, uintptr_t* end, uint8_t* knownEmpty, uint64_t* liveBytes, uint8_t* young, uint8_t* neverExam, uint8_t* auth, uint8_t* sameLife, uint32_t* takeLifeNow) { return false; }
void DumpJoinForTarget(uintptr_t tgt, const char* tag) {  }
void Report(const char* point) {  }
} // namespace RegionLifeDiag
} // namespace MapleRuntime
