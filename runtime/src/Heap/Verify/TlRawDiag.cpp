#include "Heap/Verify/TlRawDiag.h"
namespace MapleRuntime {
namespace TlRawDiag {
bool Enabled() { return false; }
void NoteMinorEnter(size_t minorRun) {  }
void NoteInitRegion(RegionInfo* region) {  }
void NoteCrashRdi(uintptr_t rdi) {  }
void Report(const char* point) {  }
} // namespace TlRawDiag
} // namespace MapleRuntime
