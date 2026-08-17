#include "Heap/Verify/NoTracedDiag.h"
namespace MapleRuntime {
namespace NoTracedDiag {
bool Enabled() { return false; }
void NoteTrace(BaseObject* obj) {  }
void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done) {  }
void NoteCrashJoin(uintptr_t holderCrash, uintptr_t holderCas) {  }
void Report(const char* point) {  }
} // namespace NoTracedDiag
} // namespace MapleRuntime
