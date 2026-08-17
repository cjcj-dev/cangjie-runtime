#include "Heap/Verify/EmptyLiveDiag.h"
namespace MapleRuntime {
namespace EmptyLiveDiag {
bool Enabled() { return false; }
void NoteCollectEnter(RegionInfo* region) {  }
void Report(const char* point) {  }
} // namespace EmptyLiveDiag
} // namespace MapleRuntime
