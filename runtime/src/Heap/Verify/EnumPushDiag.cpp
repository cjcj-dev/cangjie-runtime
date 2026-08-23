#include "Heap/Verify/EnumPushDiag.h"
namespace MapleRuntime {
namespace EnumPushDiag {
bool Enabled() { return false; }
void NoteFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, int managed, int valid, int nSlots, int nRegs, const char* funcName) {  }
void NoteMapSlot(uintptr_t fa, intptr_t bias, BaseObject* obj) {  }
void NoteMapReg(uintptr_t fa, int reg, BaseObject* obj) {  }
void NotePush(BaseObject* obj, const char* site, const void* slot) {  }
void NoteSkip(BaseObject* obj, const char* site, const char* reason) {  }
void Report(const char* point) {  }
} // namespace EnumPushDiag
} // namespace MapleRuntime
