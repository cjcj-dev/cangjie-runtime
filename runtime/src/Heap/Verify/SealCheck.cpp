#include "Heap/Verify/SealCheck.h"
namespace MapleRuntime {
namespace SealCheck {
bool Enabled() { return false; }
void NoteSeal(RegionInfo* region) {  }
void NotePaint(RegionInfo* region, size_t offset, size_t byteCnt, const char* site) {  }
void MaybeInjectLatePaint(RegionInfo* region) {  }
void DumpSummary() {  }
} // namespace SealCheck
} // namespace MapleRuntime
