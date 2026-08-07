// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STACK_FRAME_CURSOR_H
#define MRT_STACK_FRAME_CURSOR_H

#include <cstddef>
#include <vector>

#include "Common/StackType.h"
#include "StackMap/StackMapTypeDef.h"
#include "UnwindStack/StackInfo.h"

namespace MapleRuntime {

class Mutator;

// Frame-scoped stack-root cursor (stackwm Step 1).
// Pre-fills under a stable top UnwindContext (STW), then process_one / process_all
// with the same per-frame dispatch + RegSlotsMap carry as GCStackInfo::VisitStackRoots.
// Product path stays on the legacy full-stack visitor; this is the oracle substrate.
class StackFrameCursor {
public:
    explicit StackFrameCursor(const UnwindContext& topFrame);

    size_t FrameCount() const { return frames.size(); }
    size_t Cursor() const { return index; }
    bool Done() const { return index >= frames.size(); }
    const FrameInfo* CurrentFrame() const
    {
        return Done() ? nullptr : &frames[index];
    }

    // Process exactly one frame (barrier-frame or stub bookkeeping), advance cursor.
    // Returns false when already done.
    bool ProcessOne(const RootVisitor& visitor, Mutator& mutator);

    // Drain remaining frames.
    void ProcessAll(const RootVisitor& visitor, Mutator& mutator);

    // Skip the next MANAGED frame without visiting its roots (positive-control only).
    bool SkipNextManagedFrame();

    // Shared per-frame dispatch used by the legacy full-stack loop and this cursor.
    static void ProcessFrame(const FrameInfo& frame, RegSlotsMap& regSlotsMap, const RootVisitor& visitor,
                             Mutator& mutator);

private:
    std::vector<FrameInfo> frames;
    RegSlotsMap regSlotsMap;
    size_t index = 0;
};

} // namespace MapleRuntime

#endif // MRT_STACK_FRAME_CURSOR_H
