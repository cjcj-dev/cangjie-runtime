// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_INLINE_H
#define MRT_MUTATOR_INLINE_H

#include "MutatorManager.h"
#include "Base/Log.h"

namespace MapleRuntime {
inline void Mutator::RejectInconsistentUnwindTop()
{
    const bool hasFA = uwContext.frameInfo.mFrame.GetFA() != nullptr;
    const bool hasIP = uwContext.frameInfo.mFrame.GetIP() != nullptr;
    CHECK_DETAIL(hasFA == hasIP, "unwind top is missing only the frame address or only the instruction");
}

inline void Mutator::PublishUnpublishedManagedTop()
{
    RejectInconsistentUnwindTop();
    if (uwContext.GetUnwindContextStatus() == UnwindContextStatus::RISKY) {
        CHECK_DETAIL(uwContext.frameInfo.mFrame.GetFA() != nullptr, "native transition has no walkable top");
        return;
    }
    if (!IsManagedContext() || uwContext.frameInfo.mFrame.GetFA() != nullptr) {
        return;
    }
    UpdateUnwindContext();
}

inline void Mutator::DoEnterSaferegion()
{
    CheckSafepointAllowed();
    SetInSaferegion(SAFE_REGION_TRUE);
    MarkFlushOnEnterSaferegion();
}

inline bool Mutator::EnterSaferegion(bool updateUnwindContext) noexcept
{
    if (LIKELY(!InSaferegion())) {
        // RISKY already stored the native frame. A cleared managed top has not.
        if (updateUnwindContext && uwContext.GetUnwindContextStatus() != UnwindContextStatus::RISKY) {
            RejectInconsistentUnwindTop();
            UpdateUnwindContext();
        } else {
            PublishUnpublishedManagedTop();
        }
        DoEnterSaferegion();
        return true;
    }
    return false;
}

inline bool Mutator::LeaveSaferegion() noexcept
{
    if (LIKELY(InSaferegion())) {
        DoLeaveSaferegion();
        return true;
    }
    return false;
}

} // namespace MapleRuntime

#endif // MRT_MUTATOR_INLINE_H
