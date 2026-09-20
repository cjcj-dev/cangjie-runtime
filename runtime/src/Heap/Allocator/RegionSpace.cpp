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

const ZStatCriticalPhase PReclaimGarbageRegions("ReclaimGarbageRegions");
RegionManager& RegionSpace::GetRegionManager() const noexcept
{
    return Heap::GetHeap().page_allocator();
}

void RegionSpace::Init(const HeapParam& param)
{
    GetRegionManager().Init(param);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpace::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif


} // namespace MapleRuntime

#include "Base/ImmortalWrapper.h"
namespace MapleRuntime {
// PagePool
PagePool& PagePool::Instance() noexcept
{
    static ImmortalWrapper<PagePool> instance("PagePool");
    return *instance;
}


}
