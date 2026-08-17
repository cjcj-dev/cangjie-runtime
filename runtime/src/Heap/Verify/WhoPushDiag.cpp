#include "Heap/Verify/WhoPushDiag.h"
namespace MapleRuntime {
namespace WhoPushDiag {
bool Enabled() { return false; }
void NotePush(BaseObject* object, const char* site, const void* slot , BaseObject* holder ) {  }
void NoteCrashRdi(uintptr_t rdi) {  }
void Report(const char* point) {  }
} // namespace WhoPushDiag
} // namespace MapleRuntime
