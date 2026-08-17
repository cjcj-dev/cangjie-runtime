#include "Heap/Verify/DiffPathExplainer.h"
namespace MapleRuntime {
void RunDiffPathExplainer(size_t minorRunIndex, const std::function<void(const std::function<void(BaseObject*)>&)>& visitRoots, const std::function<BaseObject*(RefField<>&)>& resolveField, const std::unordered_set<MAddress>& remsetSlots, const std::unordered_set<MAddress>& consumedSlots, const std::unordered_set<RegionInfo*>* candidateRegions, const DiffPathRemsetStats& remsetStats, std::unordered_set<BaseObject*>* rootReachableOut) {  }
void ReportRemsetConsumeStats(size_t minorRunIndex, const DiffPathRemsetStats& stats) {  }
} // namespace MapleRuntime
