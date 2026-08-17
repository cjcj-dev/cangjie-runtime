#include "Heap/Verify/YyEdgeDiag.h"
namespace MapleRuntime {
namespace YyEdgeDiag {
bool Enabled() { return false; }
bool RecordEnabled() { return false; }
void NoteYoungToYoung(BaseObject* holder, MAddress fieldAddress, BaseObject* ref) {  }
void PublishProductVec(const std::vector<BaseObject*>& reachableVec) {  }
bool HolderInThisProductVec(BaseObject* holder) { return false; }
bool HolderInPrevProductVec(BaseObject* holder) { return false; }
void Report(const char* point) {  }
} // namespace YyEdgeDiag
} // namespace MapleRuntime
