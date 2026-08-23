#include "Heap/Verify/GateDropDiag.h"
namespace MapleRuntime {
namespace GateDropDiag {
bool Enabled() { return false; }
void NoteReject(BaseObject* holder, void* fieldPtr, BaseObject* target, uint8_t arm) {  }
void NoteCrashJoin(uintptr_t holder, uintptr_t slotBytes, uintptr_t tgtPeeled) {  }
void Report(const char* point) {  }
} // namespace GateDropDiag
} // namespace MapleRuntime
