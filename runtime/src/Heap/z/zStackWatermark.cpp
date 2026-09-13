// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zStackWatermark.hpp"

#include <cstdlib>
#include <cstring>

namespace MapleRuntime {


} // namespace MapleRuntime

namespace MapleRuntime {
StackWatermark::StackWatermark() { Reset(); }
}

namespace MapleRuntime {
bool StackWatermark::IsDone() const { return GetPhase() == WM_DONE; }
}

namespace MapleRuntime {
bool StackWatermark::IsDone(uint64_t scanEpoch, ProcessingPhase workPhase ) const
    {
        return GetPhase() == WM_DONE && GetEpoch() == scanEpoch &&
            processingPhase.load(std::memory_order_acquire) == workPhase && complete.load(std::memory_order_acquire);
    }
}
