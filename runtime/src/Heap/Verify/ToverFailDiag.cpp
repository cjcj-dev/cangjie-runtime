#include "Heap/Verify/ToverFailDiag.h"
namespace MapleRuntime {
namespace ToverFailDiag {
bool Enabled() { return false; }
void NoteSlowEnter() {  }
void NoteLoadGoodFast() {  }
void NoteUnmovableSkip(BaseObject* oldTarget, unsigned stateCode, unsigned isForwarded) {  }
void NoteResolveEnter() {  }
void NoteResolveOutcome(BaseObject* oldTarget, BaseObject* loadGood, unsigned moved) {  }
void NoteMlgEnter() {  }
void NoteMlgKeepFrom() {  }
void NoteMlgMoved() {  }
void NoteRemapCall() {  }
void NoteRemapNonHeap() {  }
void NoteRemapNoGhost() {  }
void NoteRemapRouteNull() {  }
void NoteRemapReceipt() {  }
void NoteRemapWait() {  }
void NoteRemapWaitTip() {  }
void NoteRemapWaitGiveUp() {  }
void NoteFwdEnter() {  }
void NoteFwdOk() {  }
void NoteFwdNull() {  }
void NoteFwdSame() {  }
void Report(const char* why) {  }
} // namespace ToverFailDiag
} // namespace MapleRuntime
