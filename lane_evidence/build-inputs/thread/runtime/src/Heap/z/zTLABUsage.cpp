// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Common/ColourEncoding.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
void AllocBuffer::AccumulateTLABStatistics(TLABStatistics& total, size_t used, size_t capacity)
{
    const size_t requested = tlabStatistics.Used();
    if (requested != 0) {
        if (used > 0.5 * capacity) {
            tlabAllocationFraction.Sample(std::min(static_cast<double>(requested) /
                                                   std::max(capacity, size_t{1}), 1.0));
        }
        tlabStatistics.allocatingThreads = 1;
    }
    tlabStatistics.refills = tlabRefills.exchange(0, std::memory_order_relaxed);
    total.Update(tlabStatistics);
    tlabStatistics = TLABStatistics{};
}

// ThreadLocalAllocBuffer::resize (threadLocalAllocBuffer.cpp:161).
}
