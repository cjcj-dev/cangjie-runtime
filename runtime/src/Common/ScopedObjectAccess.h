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
        : mutator(mutator), preprocess(preprocess), saved(mutator.GetUnwindContext())
    {
        // javaFrameAnchor.hpp:59-66 / frame_x86.cpp:710-716: a non-empty anchor is
        // not proof the frame is still the caller of this block. Refresh every
        // managed entry. RISKY is the C2N anchor; do not replace it.
        // interfaceSupport.inline.hpp:213-223: leave and process while that caller
        // is alive, then drop this capture so it cannot outlive the return.
        UnwindContext& context = mutator.GetUnwindContext();
        published = mutator.IsManagedContext() &&
            context.GetUnwindContextStatus() != UnwindContextStatus::RISKY;
        if (published) {
            mutator.UpdateUnwindContext();
        }
        (void)mutator.EnterSaferegion(false);
    }
    ~MutatorBlockInVMPreprocess()
    {
        mutator.DoLeaveSaferegion(preprocess);
        if (published) {
            mutator.GetUnwindContext() = saved;
        }
    }
    MutatorBlockInVMPreprocess(const MutatorBlockInVMPreprocess&) = delete;
    MutatorBlockInVMPreprocess& operator=(const MutatorBlockInVMPreprocess&) = delete;
private:
    Mutator& mutator;
    Preprocess& preprocess;
    UnwindContext saved;
    bool published = false;
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
