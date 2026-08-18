#include "Heap/Verify/MutatorRelocate.h"
namespace MapleRuntime {
namespace MutatorRelocate {
bool Enabled() { return kMutatorSelfRelocate; }
bool DrainEnabled() { return false; }
bool StatsOn() { return false; }
bool InjectOn() { return false; }
void NoteAttempt() {  }
void NoteRetainOk() {  }
void NoteFallback(Fallback why) {  }
void NoteAlreadyForwarded() {  }
bool InScope() { return false; }
void EnterScope() {  }
void LeaveScope() {  }
void NoteSelfCopy(size_t bytes, Role role) {  }
void NoteAnyCopy(Role role) {  }
void NoteFunnelCall(Role role) {  }
void NoteWaitEnter() {  }
void NoteWaitGiveUp() {  }
void NoteWaitReceipt() {  }
void NoteWaitFatal() {  }
void NoteDrain(Retire site, uint64_t spunNanos, bool contended) {  }
void DumpSummary() {  }
} // namespace MutatorRelocate
} // namespace MapleRuntime
