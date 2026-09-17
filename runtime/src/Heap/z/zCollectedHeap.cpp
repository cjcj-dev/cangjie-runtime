// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zCollectedHeap.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/Collector/GcStats.h"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/Mutator.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {
const char* const COLLECTOR_NAME[] = { "No Collector", "Proxy Collector", "Regional-Copying Collector",
                                       "Smooth Collector" };
}

Collector::~Collector() = default;

void Collector::MarkObjectIfActive(BaseObject* object) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    ZPage* region = Heap::page(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion()) {
        MarkYoungObjectIfActive(object);
    } else {
        MarkOldObjectIfActive(object);
    }
}

// The positional table this replaced still carried names from an older phase
// enum, so indices 12, 13 and 14 printed "forward phase", "enum fix phase" and
// "trace fix phase" for POST_TRACE, PREFORWARD and FORWARD. Every crash report
// naming a phase past CLEAR_SATB_BUFFER therefore named the wrong one, and a
// reader comparing two reports could not tell. Switching on the enum keeps the
// name attached to the value, so adding a phase is a compile error here rather
// than a silent relabelling of the phases after it.
Collector::Collector() {}

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async)
{
    RequestGCInternal(reason, async);
}

} // namespace MapleRuntime.
