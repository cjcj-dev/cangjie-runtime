// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_INLINE_H
#define MRT_MUTATOR_INLINE_H

#include "MutatorManager.h"

namespace MapleRuntime {
inline void Mutator::RejectInconsistentUnwindTop()
{
    const bool hasFA = uwContext.frameInfo.mFrame.GetFA() != nullptr;
    const bool hasIP = uwContext.frameInfo.mFrame.GetIP() != nullptr;
    CHECK_DETAIL(hasFA == hasIP, "unwind top is missing only the frame address or only the instruction");
}

inline void Mutator::DoEnterSaferegion(HandshakeState* boundHandshake)
{
    SetInSaferegion(SAFE_REGION_TRUE);
    if (boundHandshake != nullptr) {
        boundHandshake->enter_safe();
    } else {
        MarkFlushOnEnterSaferegion();
    }
}

inline bool Mutator::EnterSaferegion(bool updateUnwindContext, HandshakeState* boundHandshake) noexcept
{
    if (LIKELY(!InSaferegion())) {
        // When the status is risky, no update is required. Because
        // valid information is already stored in the context.
        if (updateUnwindContext && uwContext.GetUnwindContextStatus() != UnwindContextStatus::RISKY) {
            UpdateUnwindContext();
        }
        DoEnterSaferegion(boundHandshake);
        return true;
    }
    return false;
}

inline bool Mutator::LeaveSaferegion(HandshakeState* boundHandshake) noexcept
{
    if (LIKELY(InSaferegion())) {
        DoLeaveSaferegion(boundHandshake);
        return true;
    }
    return false;
}

inline bool Mutator::LeaveNative(HandshakeState* boundHandshake)
{
    const bool transitioned = LeaveSaferegion(boundHandshake);
    if (transitioned) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
        // HotSpot's native-return poll compares the published stub anchor with
        // the thread-owned poll word, after processing transition requests.
        // A watermark names the iterator's callee SP: an anchor below it cannot
        // expose an unprocessed sender. An older poll boundary is conservative;
        // only this thread disarms it, while requests and rebinding arm it.
        const UnwindContext& context = GetUnwindContext();
        const uintptr_t anchor = reinterpret_cast<uintptr_t>(context.frameInfo.mFrame.GetFA());
        const uintptr_t poll = ThreadLocal::GetThreadLocalData()->GetPollWord();
        if (IsManagedContext() && context.GetUnwindContextStatus() == UnwindContextStatus::RISKY &&
            anchor != 0 && context.frameInfo.mFrame.GetIP() != nullptr &&
            (poll & ThreadLocalData::PollBit) == 0 && anchor <= poll &&
            GetStackWatermark().processing_started()) {
            return transitioned;
        }
#endif
        // HotSpot javaThread.cpp:1103-1118: process requests before exposing
        // the caller on return from native code. N2C entry does not unwind.
        StackWatermarkSet::before_unwind(*this);
    }
    return transitioned;
}

} // namespace MapleRuntime

#endif // MRT_MUTATOR_INLINE_H
