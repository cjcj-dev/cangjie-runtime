// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SCOPED_SAFEREGION_H
#define MRT_SCOPED_SAFEREGION_H

#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {
// HotSpot interfaceSupport.inline.hpp:203-223: release an in-flight lock
// before processing a request while returning from the blocked state.
template<class Preprocess>
class MutatorBlockInVMPreprocess {
public:
    MutatorBlockInVMPreprocess(Mutator& mutator, Preprocess& preprocess)
        : mutator(mutator), preprocess(preprocess)
    {
        // interfaceSupport.inline.hpp:209-211 only transitions a thread that is
        // still in the VM. An outer saferegion stays as it is.
        if (mutator.InSaferegion()) {
            return;
        }
        UnwindContext& context = mutator.GetUnwindContext();
        // frame_x86.cpp:710-716 fills the pc from the live stack before the
        // thread is blocked. RISKY is GoIntoNativeCode; do not replace it.
        // Only the overwritten frame identity is restored. anchorFA is the walk
        // stop and stack move writes it on the live context (Mutator.cpp).
        if (mutator.IsManagedContext() &&
            context.GetUnwindContextStatus() != UnwindContextStatus::RISKY) {
            mutator.RejectInconsistentUnwindTop();
            savedIP = context.frameInfo.mFrame.GetIP();
            savedFA = context.frameInfo.mFrame.GetFA();
            mutator.UpdateUnwindContext();
            published = true;
        }
        entered = mutator.EnterSaferegion(false);
    }
    ~MutatorBlockInVMPreprocess()
    {
        if (entered) {
            mutator.DoLeaveSaferegion(preprocess);
        }
        if (published) {
            UnwindContext& context = mutator.GetUnwindContext();
            context.frameInfo.mFrame.SetIP(savedIP);
            context.frameInfo.mFrame.SetFA(savedFA);
        }
    }
    MutatorBlockInVMPreprocess(const MutatorBlockInVMPreprocess&) = delete;
    MutatorBlockInVMPreprocess& operator=(const MutatorBlockInVMPreprocess&) = delete;
private:
    Mutator& mutator;
    Preprocess& preprocess;
    const uint32_t* savedIP = nullptr;
    FrameAddress* savedFA = nullptr;
    bool published = false;
    bool entered = false;
};

// Scoped guard for saferegion.
class ScopedEnterSaferegion {
public:
    ScopedEnterSaferegion() = delete;
    explicit ScopedEnterSaferegion(bool onlyForMutator = false)
    {
        Mutator* mutator = Mutator::GetMutator();
        ThreadType threadType = ThreadLocal::GetThreadType();
        if (onlyForMutator &&
            (threadType == ThreadType::FP_THREAD || threadType == ThreadType::GC_THREAD ||
             threadType == ThreadType::VM_THREAD)) {
            stateChanged = false;
        } else {
            stateChanged = (mutator != nullptr) ? mutator->EnterSaferegion(true) : false;
        }
    }

    ~ScopedEnterSaferegion()
    {
        if (LIKELY(stateChanged)) {
            Mutator* mutator = Mutator::GetMutator(); // state changed, mutator must be not null
            (void)mutator->LeaveSaferegion();
        }
    }

private:
    bool stateChanged;
};

class ScopedObjectAccess {
public:
    ScopedObjectAccess() : mutator(Mutator::GetMutator()),
                           leavedSafeRegion(mutator == nullptr ? false : mutator->LeaveSaferegion()) {}

    ~ScopedObjectAccess()
    {
        if (LIKELY(leavedSafeRegion)) {
            // qemu use c++ thread local, it has issue with some cases for ZRT annotation, if reload again
            // fail on O3 and pass on O0 if load mutator again, not figureout why yet
            (void)mutator->EnterSaferegion(false);
        }
    }

private:
    Mutator* mutator{ nullptr };
    bool leavedSafeRegion;
};
} // namespace MapleRuntime

#endif // MRT_SCOPED_SAFEREGION_H
