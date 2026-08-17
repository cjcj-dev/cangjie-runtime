#include "Heap/Verify/FysDesignDiag.h"
namespace MapleRuntime {
namespace FysDesignDiag {
bool Enabled() { return false; }
void OnMinorBegin(size_t minorRunIndex) {  }
void Census(const std::vector<BaseObject*>& reachableVec, const std::unordered_set<MAddress>& rememberedSlots, bool fullYoungScan, BaseObject* (*resolve)(RefField<>& field)) {  }
void Report(const char* tag) {  }
} // namespace FysDesignDiag
} // namespace MapleRuntime
