// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zStackWatermark.hpp"
#include "Heap/z/zAddress.hpp"

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
uint32_t StackWatermark::epoch_id()
{
    return __atomic_load_n(ZPointerStoreGoodMaskLowOrderBitsAddr, __ATOMIC_ACQUIRE);
}

bool StackWatermark::IsDone(uint64_t scanEpoch) const
{
    const uint32_t packed = state.load(std::memory_order_acquire);
    return UnpackDone(packed) && UnpackEpoch(packed) == static_cast<uint32_t>(scanEpoch);
}
}
