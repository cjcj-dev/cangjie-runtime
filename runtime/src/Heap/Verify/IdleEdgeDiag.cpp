#include "Heap/Verify/IdleEdgeDiag.h"
namespace MapleRuntime {
namespace IdleEdgeDiag {
bool Enabled() { return false; }
void NoteBarrierDecision(MAddress fieldAddress, GCPhase phase, bool recorded, uint8_t holderGen, uint8_t targetGen, uint8_t skipReason , uint8_t holderObjGen ) {  }
void CensusPrePinnedStamp(size_t minorRunIndex) {  }
void DumpProcessTotals(const char* tag) {  }
void NotePromoteTimeTarget(MAddress fieldAddress, uint8_t targetGen, bool recorded) {  }
void RunSelfTest() {  }
} // namespace IdleEdgeDiag
} // namespace MapleRuntime
