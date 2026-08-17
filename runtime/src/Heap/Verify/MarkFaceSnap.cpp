#include "Heap/Verify/MarkFaceSnap.h"
namespace MapleRuntime {
namespace MarkFaceSnap {
bool Enabled() { return false; }
void NoteRegionFree(RegionInfo* region, uint16_t path) {  }
void NoteBeforeReleaseDecision(RegionInfo* region) {  }
void DumpJoinForAddr(uintptr_t addr, const char* tag, bool deepScan ) {  }
void NoteCrashSweep(const uintptr_t* addrs, const char* const* names, size_t n) {  }
void Report(const char* point) {  }
} // namespace MarkFaceSnap
} // namespace MapleRuntime
