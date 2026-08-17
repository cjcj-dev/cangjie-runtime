#include "Heap/Verify/DiagGate.h"
namespace MapleRuntime {
namespace DiagGate {
bool TokenOn(const char* token) { return false; }
bool LegacyOrToken(const char* legacyEnv, const char* token) { return false; }
bool SelfTestOn() { return false; }
void MaybeAnnounce() {  }
void EmitCounterLegend() {  }
} // namespace DiagGate
} // namespace MapleRuntime
