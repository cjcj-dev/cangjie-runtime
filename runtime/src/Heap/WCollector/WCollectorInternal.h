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

WCOLLECTOR_INTERNAL_HIDDEN void VerifyStackRootPostcondition(uint64_t stackScanEpoch, const char* source);
WCOLLECTOR_INTERNAL_HIDDEN void PushAdmittedYoung(BaseObject* object, TracingCollector::WorkStack& workStack,
                                                  const char* origin, const void* slot = nullptr,
                                                  BaseObject* holder = nullptr);
WCOLLECTOR_INTERNAL_HIDDEN void PushAdmittedYoung(const MarkStackEntry& entry,
                                                  TracingCollector::WorkStack& workStack,
                                                  const char* origin, const void* slot = nullptr,
                                                  BaseObject* holder = nullptr);
WCOLLECTOR_INTERNAL_HIDDEN bool ScrubMinorFreeTarget(RefField<>& field, BaseObject* target, bool fromFix,
                                                    bool holderIsCurrentMinorRoot = false);

WCOLLECTOR_INTERNAL_HIDDEN bool HolderObjectIsLive(BaseObject* holder);
WCOLLECTOR_INTERNAL_HIDDEN bool SlotHeldByLiveObject(const void* slot);
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
