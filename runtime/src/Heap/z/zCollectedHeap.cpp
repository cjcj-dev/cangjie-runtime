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

#include "Base/ImmortalWrapper.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Heap/z/zStat.hpp"
#include "Common/BaseObject.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/StateWord.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zAbort.hpp"
#include "Heap/z/zDirector.hpp"
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

static ImmortalWrapper<ZCollectedHeap> g_collectedHeap;

ZCollectedHeap* ZCollectedHeap::heap() { return &*g_collectedHeap; }

ZCollectedHeap::ZCollectedHeap()
    : _heap(),
      _driver_minor(nullptr),
      _driver_major(nullptr),
      _director(nullptr),
      _stat(nullptr),
      _runtime_workers()
{
}

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

const char* Collector::GetCollectorName() const { return COLLECTOR_NAME[collectorType]; }

void Collector::RequestGC(GCReason reason, bool async)
{
    RequestGCInternal(reason, async);
}

void ZCollectedHeap::stop()
{
    ZAbort::abort();
    Heap::GetHeap().GetCollectorResources().StopGCWork();
}

} // namespace MapleRuntime
