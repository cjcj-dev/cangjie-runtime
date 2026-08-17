#include "Heap/Verify/FwdInflight.h"
namespace MapleRuntime {
namespace FwdInflight {
bool Enabled() { return false; }
bool InjectOn() { return false; }
Scope::Scope(const RegionInfo* region, Site site) {}
Scope::~Scope() {}
void NoteRetireRegion(const RegionInfo* region, Retire retire) {  }
void NoteRetireGlobal(uintptr_t rangeStart, size_t rangeSize, Retire retire) {  }
void DumpSummary() {  }
} // namespace FwdInflight
} // namespace MapleRuntime
