#include "Heap/Verify/GoodPredDiag.h"
namespace MapleRuntime {
namespace GoodPredDiag {
uint8_t g_mode = kZgc;
bool g_applyZgc = true;
bool NoteAudit(uintptr_t value, bool legacy, bool zgc, uint8_t site) { return false; }
bool SelfTestPending() { return false; }
void ReportSelfTest(uintptr_t taggedValue, bool taggedLegacy, bool taggedZgc, uintptr_t plainValue, bool plainLegacy, bool plainZgc) {  }
void Report(const char* why) {  }
} // namespace GoodPredDiag
} // namespace MapleRuntime
