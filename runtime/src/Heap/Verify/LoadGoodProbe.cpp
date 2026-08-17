#include "Heap/Verify/LoadGoodProbe.h"
namespace MapleRuntime {
namespace LoadGoodProbe {
bool g_enabled = false;
void NoteNull(uint8_t face) {  }
void NoteRead(uint8_t face, uintptr_t word, bool ghost, bool loadGood) {  }
void NoteBadSample(uint8_t face, uintptr_t word, uintptr_t stripped) {  }
void NoteRouteSample(uint8_t face, uintptr_t word, uintptr_t stripped, uintptr_t resolved) {  }
void Report(const char* point) {  }
} // namespace LoadGoodProbe
} // namespace MapleRuntime
