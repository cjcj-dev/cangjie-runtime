#include "Heap/Verify/HdrWhoDiag.h"
namespace MapleRuntime {
namespace HdrWhoDiag {
bool Enabled() { return false; }
void NoteCrashRdi(uintptr_t rdi) {  }
} // namespace HdrWhoDiag
} // namespace MapleRuntime
