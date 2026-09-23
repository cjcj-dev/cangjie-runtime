// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_MANAGER_INLINE_H
#define MRT_HEAP_MANAGER_INLINE_H

// inline managed-heap functions
// common headers
#include "Base/Globals.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "schedule.h"
// module internal headers
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "HeapManager.h"

namespace MapleRuntime {
inline void HeapManager::RequestGC(GCReason reason)
{
    if (!Heap::GetHeap().IsGCEnabled()) {
        return;
    }
    Heap::GetHeap().RequestGC(reason);
}
} // namespace MapleRuntime

#endif // MRT_HEAP_MANAGER_INLINE_H
