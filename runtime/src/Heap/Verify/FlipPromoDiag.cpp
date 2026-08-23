#include "Heap/Verify/FlipPromoDiag.h"
namespace MapleRuntime {
namespace FlipPromoDiag {
bool Enabled() { return false; }
void NoteProductRecord(MAddress slot, unsigned path) {  }
void NotePromotedRegion(RegionInfo* region, unsigned path, size_t productRecorded) {  }
void OnBroadScanBegin(size_t minorRunIndex) {  }
void NoteBroadRecord(RegionInfo* holderRegion, MAddress slot) {  }
void OnPromotePhaseEnd(size_t minorRunIndex, size_t promoteReplay, size_t residualPromote) {  }
void DumpProcessTotals(const char* tag) {  }
} // namespace FlipPromoDiag
} // namespace MapleRuntime
