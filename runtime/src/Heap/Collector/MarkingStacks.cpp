// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Collector/MarkingStacks.h"
#include "Heap/Verify/ZVerify.h"
namespace MapleRuntime {
namespace MarkingStacks {
void VerifyEmpty(size_t pending)
{
    if (ZVerifyMarking) { CHECK_DETAIL(pending == 0, "Marking stack is not empty: %zu", pending); }
}
}
}
