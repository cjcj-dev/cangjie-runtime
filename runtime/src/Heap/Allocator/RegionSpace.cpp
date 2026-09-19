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

const ZStatSubPhase OldForwardFromRegions("ForwardFromRegions", ZGenerationId::old);
const ZStatSubPhase PExemptFromRegions("ExemptFromRegions", ZGenerationId::old);
const ZStatCriticalPhase PReclaimGarbageRegions("ReclaimGarbageRegions");
const ZStatSubPhase YoungForwardFromRegions("ForwardFromRegions", ZGenerationId::young);
RegionManager& RegionSpace::GetRegionManager() const noexcept
{
    return Heap::GetHeap().page_allocator();
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpace::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif
void RegionSpace::FeedHungryBuffers()
{
    ScopedObjectAccess soa;
    AllocBufferManager::HungryBuffers hungryBuffers;
    allocBufferManager->SwapHungryBuffers(hungryBuffers);
    for (auto* buffer : hungryBuffers) {
        if (buffer->GetPreparedRegion() != nullptr) { continue; }
        ZPage* region = GetRegionManager().AllocateThreadLocalRegion(
            buffer->ComputeTLABSize(0, GetRegionManager().GetThreadLocalRegionSize()), true);
        if (region == nullptr) { return; }
        if (!buffer->SetPreparedRegion(region)) {
            // This extent was computed for this buffer's history. Return it
            // instead of handing that thread's size to another buffer.
            GetRegionManager().UndoThreadLocalRegionAllocation(region);
        }
    }
}

} // namespace MapleRuntime
