// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MAPLE_RUNTIME_WCOLLECTOR_INTERNAL_H
#define MAPLE_RUNTIME_WCOLLECTOR_INTERNAL_H

#include <atomic>
#include <cstdint>

#if defined(__GNUC__)
#define WCOLLECTOR_INTERNAL_HIDDEN __attribute__((visibility("hidden")))
#else
#define WCOLLECTOR_INTERNAL_HIDDEN
#endif

namespace MapleRuntime {
namespace WCollectorInternal {

WCOLLECTOR_INTERNAL_HIDDEN bool MinorYoungFlipOff();
WCOLLECTOR_INTERNAL_HIDDEN bool NullslotProbeEnabled();
WCOLLECTOR_INTERNAL_HIDDEN void VerifyStackRootPostcondition(uint64_t stackScanEpoch, const char* source);
WCOLLECTOR_INTERNAL_HIDDEN void PushAdmittedYoung(BaseObject* object, TracingCollector::WorkStack& workStack,
                                                  const char* origin, const void* slot = nullptr,
                                                  BaseObject* holder = nullptr);
WCOLLECTOR_INTERNAL_HIDDEN void PushAdmittedYoung(const MarkStackEntry& entry,
                                                  TracingCollector::WorkStack& workStack,
                                                  const char* origin, const void* slot = nullptr,
                                                  BaseObject* holder = nullptr);
WCOLLECTOR_INTERNAL_HIDDEN bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool fromFix);

extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_nullslotF3;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_nullslotResolve;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_nullslotRemset;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_nullslotResolveRoot;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_resolveRootEntry;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_resolveRootOld;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_resolveRootHealNull;
extern WCOLLECTOR_INTERNAL_HIDDEN std::atomic<size_t> g_fixMinorRootSlotsCalls;

WCOLLECTOR_INTERNAL_HIDDEN bool HolderObjectIsLive(BaseObject* holder);
WCOLLECTOR_INTERNAL_HIDDEN bool SlotHeldByLiveObject(const void* slot);
WCOLLECTOR_INTERNAL_HIDDEN void ReportF3DeadarmCounts(const char* point);
WCOLLECTOR_INTERNAL_HIDDEN const char* NoteF3DeadarmHit(const char* reason, BaseObject* holder);
WCOLLECTOR_INTERNAL_HIDDEN void NoteNullslotWrite(const char* path, BaseObject* holder, void* field,
                                                 BaseObject* from, BaseObject* latest,
                                                 std::atomic<size_t>* pathCount);
WCOLLECTOR_INTERNAL_HIDDEN const char* ClassifyRootLiveFail(BaseObject* obj, RegionInfo* region);
WCOLLECTOR_INTERNAL_HIDDEN void NoteResolveRootNull(void* rootSlot, BaseObject* from, BaseObject* to,
                                                   RegionInfo* fromRegion, RegionInfo* toRegion,
                                                   const char* toWhy, const char* fromWhy);

template <typename SetT, typename KeyT>
bool LedgerInsert(SetT& set, const KeyT& key)
{
    return set.insert(key).second;
}

template <typename SetT, typename KeyT>
size_t LedgerCount(const SetT& set, const KeyT& key)
{
    return set.count(key);
}

} // namespace WCollectorInternal

using namespace WCollectorInternal;
} // namespace MapleRuntime

#undef WCOLLECTOR_INTERNAL_HIDDEN

#endif // MAPLE_RUNTIME_WCOLLECTOR_INTERNAL_H
