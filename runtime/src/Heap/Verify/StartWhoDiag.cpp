#include "Heap/Verify/StartWhoDiag.h"
namespace MapleRuntime {
namespace StartWhoDiag {
bool Enabled() { return false; }
void NoteRootCandidate(BaseObject* object, const char* site, const void* slot, BaseObject* base, BaseObject* derived) {  }
void NoteProducedRootIfPending(BaseObject* object) {  }
void DiscardRootCandidate(BaseObject* object) {  }
void NoteProduced(BaseObject* object, Source source, const char* site, const void* slot , BaseObject* holder ) {  }
ScopedCaller::ScopedCaller(const char* caller, BaseObject* object) {}
ScopedCaller::~ScopedCaller() {}
void NoteCrash() {  }
void Report(const char* point) {  }
} // namespace StartWhoDiag
} // namespace MapleRuntime
