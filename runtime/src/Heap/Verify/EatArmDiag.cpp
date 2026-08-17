#include "Heap/Verify/EatArmDiag.h"
namespace MapleRuntime {
namespace EatArmDiag {
bool Enabled() { return false; }
void OnMinorBegin(size_t minorRunIndex) {  }
void NoteWasMarkedSkipFields(BaseObject* holder) {  }
void NoteNonYoungDedupSkipFields(BaseObject* holder) {  }
void NoteFysRemsetSkip(MAddress slot, BaseObject* target) {  }
void NoteFixpointEdge(BaseObject* holder, BaseObject* target, FixpointReason reason) {  }
void SetFixHost(BaseObject* host) {  }
BaseObject* GetFixHost() { return nullptr; }
void NoteIorTarget(BaseObject* targetT, BaseObject* host, size_t fieldOff) {  }
void DumpMinorSummary(size_t minorRunIndex) {  }
void RunSelfTest() {  }
} // namespace EatArmDiag
} // namespace MapleRuntime
