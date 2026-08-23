#include "Heap/Verify/FysAuditDiag.h"
namespace MapleRuntime {
namespace FysAuditDiag {
bool Enabled() { return false; }
bool ForceProductFullYoungScanFalse() { return false; }
void OnMinorBegin(size_t minorRunIndex) {  }
void CensusPrePinned(size_t minorRunIndex) {  }
void CensusPostPinned(size_t minorRunIndex, size_t pinnedRecorded) {  }
void PostRescan(const std::unordered_set<MAddress>& rememberedSlots, const std::unordered_set<MAddress>& liveRememberedSlots, const std::unordered_set<MAddress>& consumedSlots, const std::unordered_set<MAddress>& weakSlots) {  }
void Report(const char* tag) {  }
void DumpProcessTotals(const char* tag) {  }
} // namespace FysAuditDiag
} // namespace MapleRuntime
