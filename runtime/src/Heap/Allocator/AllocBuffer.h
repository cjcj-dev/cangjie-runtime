// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#pragma once


namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
inline void AllocBuffer::SetY2yDirtyHolderMergeHookForTest(Y2yDirtyHolderMergeHook hook, void* context) {
        std::lock_guard<std::mutex> lock(y2yDirtyLock);
        y2yDirtyHolderMergeHook = hook;
        y2yDirtyHolderMergeHookContext = context;
    }
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
#endif
} // namespace MapleRuntime

namespace MapleRuntime {
#if defined(MRT_GC_UNIT_TESTS)
inline void AllocBuffer::FireHandoffHook(HandoffHook hook, void* context) {
        if (hook != nullptr) {
            hook(context);
        }
    }
#endif
} // namespace MapleRuntime
