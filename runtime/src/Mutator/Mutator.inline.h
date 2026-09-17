// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_INLINE_H
#define MRT_MUTATOR_INLINE_H

#include "MutatorManager.h"

namespace MapleRuntime {
inline void Mutator::DoEnterSaferegion()
{
    SetInSaferegion(SAFE_REGION_TRUE);
    MarkFlushOnEnterSaferegion();
}

inline bool Mutator::EnterSaferegion(bool updateUnwindContext) noexcept
{
    if (LIKELY(!InSaferegion())) {
        // When the status is risky, no update is required. Because
        // valid information is already stored in the context.
        if (updateUnwindContext && uwContext.GetUnwindContextStatus() != UnwindContextStatus::RISKY) {
            UpdateUnwindContext();
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
