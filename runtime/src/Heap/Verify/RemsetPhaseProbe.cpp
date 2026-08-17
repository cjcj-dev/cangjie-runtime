#include "Heap/Verify/RemsetPhaseProbe.h"
namespace MapleRuntime {
namespace RemsetPhaseProbe {
bool Enabled() { return false; }
BarrierClass PhaseToBarrierClass(GCPhase phase) { return {}; }
const char* PhaseName(GCPhase phase) { return nullptr; }
const char* BarrierClassName(BarrierClass bc) { return nullptr; }
const char* SkipReasonName(SkipReason r) { return nullptr; }
void NoteWrite(MAddress fieldAddress, GCPhase phase, SkipReason reason, bool recorded) {  }
void NoteMissing(MAddress fieldAddress) {  }
void DumpSummary(const char* tag) {  }
void ClearSlotStamps() {  }
bool ForceRecordEnabled() { return false; }
} // namespace RemsetPhaseProbe
} // namespace MapleRuntime
