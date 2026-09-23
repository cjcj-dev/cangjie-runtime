// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Base/Semaphore.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {
// runtime/semaphore.inline.hpp:33-41: allow safepoints only while blocked.
inline void Semaphore::wait_with_safepoint_check()
{
    ScopedEnterSaferegion blocked(false);
    wait();
}
} // namespace MapleRuntime
